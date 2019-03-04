#include <sys/types.h>

#include <sys/socket.h>

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* from bitorrent - libutp */
#include <utp.h>

/* my utility library */
#include <rho/rho.h>

#define MIN(a,b) ((a) > (b)) ? (b) : (a)

/* this macro is just for documentation purposes; libutp
 * generally ignores the value a callback returns
 */
#define XUTP_RETURN_IGNORED(val) return ((val));

#define XUTP_ESUCCESS       0
#define XUTP_ECONNREFUSED   (-1)
#define XUTP_ECONNRESET     (-2)
#define XUTP_ETIMEDOUT      (-3)
#define XUTP_ENOTCONN       (-4)
#define XUTP_EISCONN        (-5)
#define XUTP_EBADF          (-10)
#define XUTP_EINIT          (-254)     /* libutp context not created */
#define XUTP_EUNKNOWN       (-255)

#define XUTP_POLL_TIMEOUT 500
#define XUTP_POLLARRAY_INITCAPACITY 16

struct xutp_conn {
    int xfd;
    RHO_SIMPLEQ_ENTRY(xutp_conn) next;
};

/* just declares type 'struct xutp_connq' */
RHO_SIMPLEQ_HEAD(xutp_connq, xutp_conn);  

enum xutp_cud_type {
    XUTP_CUD_TYPE_ACTIVE_CLIENT,
    XUTP_CUD_TYPE_PASSIVE_SERVER,
    XUTP_CUD_TYPE_ACTIVE_SERVER
};

/* TODO: consider putting connq here rather than in sud */
struct xutp_cud {
   /* pointer back to ctx that the cud is attached to */
    utp_context         *ctx;
     /* underlying udp fd */
    int                 fd;
    enum xutp_cud_type type;
    int                 refcnt;
    /* list of xfds that get accepted on a listening socket */
    struct xutp_connq   connq; 
    pthread_mutex_t     lock;
    pthread_cond_t      cv;
    RHO_LIST_ENTRY(xutp_cud) next;
};

struct xutp_mbuf {
    unsigned char *buf;
    size_t  off;
    size_t  buflen;
    RHO_SIMPLEQ_ENTRY(xutp_mbuf) next; 
};

/* just declares type 'struct xutp_mbufq' */
RHO_SIMPLEQ_HEAD(xutp_mbufq, xutp_mbuf);

enum xutp_state {
    XUTP_STATE_UNINITIALIZED = 0,
    XUTP_STATE_CONNECTED,
    XUTP_STATE_LISTENING,
    XUTP_STATE_WRITABLE,
    XUTP_STATE_CLOSED,
    XUTP_STATE_ERROR
};

/* utp_socket usedata */
struct xutp_sud {
    int             xfd;

    utp_socket      *sk;
    utp_context     *ctx;

    enum xutp_state state;
    /* if state == XUTP_STATE_ERROR */
    int             err;

    pthread_mutex_t lock;
    pthread_cond_t  cv;

    struct sockaddr_storage laddr;
    socklen_t               lalen;

    struct xutp_mbufq   mbufq;
    bool blocking;
    struct timespec timeout;
    RHO_LIST_ENTRY(xutp_sud) next;
};

/* just declares type 'struct xutp_sudtable' */
RHO_LIST_HEAD(xutp_sudtable, xutp_sud);

/* just declares type 'struct xutp_cudtable' */
RHO_LIST_HEAD(xutp_cudtable, xutp_cud);

struct xutp_pollarray {
    struct pollfd   *pollfds;
    struct xutp_cud **cuds;
    nfds_t  nfds;
    nfds_t  capacity;
};

/**********************************************
 * GLOBALS
 ***********************************************/
static struct xutp_cudtable xutp_cudtable = RHO_LIST_HEAD_INITIALIZER(&xutp_cudtable);
static pthread_mutex_t xutp_cudtable_lock = PTHREAD_MUTEX_INITIALIZER;

static struct xutp_sudtable xutp_sudtable = RHO_LIST_HEAD_INITIALIZER(&xutp_sudtable);
static int xutp_xfdcnt = 0;
static pthread_mutex_t xutp_sudtable_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t xutp_netthread;
static volatile bool xutp_netloop_started = false;
static volatile bool xutp_fini_called = false;

/**********************************************
 * FORWARD DECLARATIONS
 ***********************************************/
static struct xutp_conn * xutp_conn_create(int xfd);
static void xutp_conn_destroy(struct xutp_conn *conn);
static void xutp_connq_destroy(struct xutp_connq *head);

static struct xutp_mbuf * xutp_mbuf_create(const void *buf, size_t len);
static void xutp_mbuf_destroy(struct xutp_mbuf *mbuf);
static void xutp_mbufq_destroy(struct xutp_mbufq *head) ;

static struct xutp_cud * xutp_cud_create(utp_context *ctx, int fd);
static void xutp_cud_destroy(struct xutp_cud *cud);
static void xutp_cud_recv(struct xutp_cud *cud);
static void xutp_cud_pushconn(struct xutp_cud *cud, struct xutp_conn *conn);
static struct xutp_conn * xutp_cud_popconn(struct xutp_cud *cud);

static void xutp_cudtable_destroy(struct xutp_cudtable *head);
static void xutp_cudtable_insert(struct xutp_cud *cud);

/* defined but not used */
#if 0
static struct xutp_cud * xutp_fd2cud(int fd);
#endif

static struct xutp_sud * xutp_sud_create(utp_context *ctx);
static void xutp_sud_destroy(struct xutp_sud *sud);
static void xutp_sud_pushmbuf(struct xutp_sud *sud, struct xutp_mbuf *mbuf);
static void xutp_sud_changestate(struct xutp_sud *sud, int state);
static int xutp_sud_awaitconnected(struct xutp_sud *sud);

/* defined but not used */
#if 0
static bool xutp_sud_awaitwritable(struct xutp_sud *sud);
#endif

ssize_t xutp_sud_awaitrecv(struct xutp_sud *sud, void *buf, size_t len);

static void xutp_sudtable_destroy(struct xutp_sudtable *head);
static int xutp_sudtable_insert(struct xutp_sud *sud);
static struct xutp_sud * xutp_xfd2sud(int xfd);

static struct xutp_pollarray * xutp_pollarray_create(void);
static void xutp_pollarray_destroy(struct xutp_pollarray *pa);
static void xutp_pollarray_push(struct xutp_pollarray *pa,
        struct xutp_cud *cud, short events);
static void xutp_pollarray_clear(struct xutp_pollarray *pa);

static uint64 xutp_log(utp_callback_arguments *a);
static uint64 xutp_on_error(utp_callback_arguments *a);
static uint64 xutp_on_firewall(utp_callback_arguments *a);
static uint64 xutp_on_state_change(utp_callback_arguments *a);
static uint64 xutp_on_connect(utp_callback_arguments *a);
static uint64 xutp_on_accept(utp_callback_arguments *a);
static uint64 xutp_on_read(utp_callback_arguments *a);
static uint64 xutp_on_sendto(utp_callback_arguments *a);

static void * xutp_netloop(void *arg);

/**********************************************
 * XUTP_MBUF / XUTP_MBUFQ
 ***********************************************/
static struct xutp_mbuf *
xutp_mbuf_create(const void *buf, size_t len)
{
    struct xutp_mbuf *mbuf = NULL;
    
    /* XXX: don't need two callocs */
    mbuf = rhoL_zalloc(sizeof(*mbuf));
    mbuf->buf = rhoL_malloc(len);
    mbuf->buflen = len;
    memcpy(mbuf->buf, buf, len);

    return (mbuf);
}

static void
xutp_mbuf_destroy(struct xutp_mbuf *mbuf)
{
    free(mbuf->buf);
    free(mbuf);
}

static void
xutp_mbufq_destroy(struct xutp_mbufq *head)
{
    struct xutp_mbuf *mbuf = NULL;

    while ((mbuf = RHO_SIMPLEQ_FIRST(head)) != NULL) {
        RHO_SIMPLEQ_REMOVE_HEAD(head, next);
        xutp_mbuf_destroy(mbuf);
    }
}

/**********************************************
 * XUTP_CONN / XUTP_CONNQ
 ***********************************************/
static struct xutp_conn *
xutp_conn_create(int xfd)
{
    struct xutp_conn *conn = NULL;

    RHO_TRACE_ENTER();

    conn = rhoL_calloc(1, sizeof(*conn));
    conn->xfd = xfd;

    RHO_TRACE_EXIT();
    return (conn);
}

static void
xutp_conn_destroy(struct xutp_conn *conn)
{
    RHO_TRACE_ENTER();

    free(conn);

    RHO_TRACE_EXIT();
}

static void
xutp_connq_destroy(struct xutp_connq *head)
{
    struct xutp_conn *conn = NULL;

    RHO_TRACE_ENTER();

    while ((conn = RHO_SIMPLEQ_FIRST(head)) != NULL) {
        RHO_SIMPLEQ_REMOVE_HEAD(head, next);
        xutp_conn_destroy(conn);
    }

    RHO_TRACE_EXIT();
}

/**********************************************
 * XUTP_CUD (the utp_[c]ocket [u]ser [d]ata)
 * XUTP_CUDTABLE
 ***********************************************/
struct xutp_cud *
xutp_cud_create(utp_context *ctx, int fd)
{
    struct xutp_cud *cud = NULL;

    RHO_TRACE_ENTER();

    cud = rhoL_calloc(1, sizeof(*cud));
    cud->ctx = ctx;
    cud->fd = fd;
    cud->refcnt = 1;
    RHO_SIMPLEQ_INIT(&cud->connq);
    rhoL_pthread_mutex_init(&cud->lock, NULL);
    rhoL_pthread_cond_init(&cud->cv, NULL);

    utp_context_set_userdata(ctx, cud);

    RHO_TRACE_EXIT();

    return (cud);
}

static void
xutp_cud_destroy(struct xutp_cud *cud)
{
    RHO_TRACE_ENTER();

    cud->refcnt--;

    if (cud->refcnt <= 0) {
        pthread_mutex_destroy(&cud->lock);
        pthread_cond_destroy(&cud->cv);
        xutp_connq_destroy(&cud->connq);
        free(cud);
    }

    RHO_TRACE_EXIT();
}

static void
xutp_cud_recv(struct xutp_cud *cud)
{
	struct sockaddr_in addr;
	socklen_t alen = sizeof(addr);
    uint8_t buf[4096] = { 0 };
	ssize_t len = 0;

    RHO_TRACE_ENTER();  

    while (1) {
        alen = sizeof(addr);
        len = recvfrom(cud->fd, buf, sizeof(buf), MSG_DONTWAIT,
            (struct sockaddr *)&addr, &alen);
        if (len == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                utp_issue_deferred_acks(cud->ctx);
                goto done;
            } else {
                rho_errno_die(errno, "recvfrom");
            }
        }
        
        //fprintf(stderr, "utp_process_udp()\n");
	    utp_process_udp(cud->ctx, buf, len, (struct sockaddr *)&addr, alen);

        /* TODO: need to exit this loop */
    }

done:
    RHO_TRACE_EXIT();  
}

static void
xutp_cud_pushconn(struct xutp_cud *cud, struct xutp_conn *conn)
{
    RHO_TRACE_ENTER();

    rhoL_pthread_mutex_lock(&cud->lock);
    RHO_SIMPLEQ_INSERT_TAIL(&cud->connq, conn, next);
    rhoL_pthread_cond_signal(&cud->cv);
    rhoL_pthread_mutex_unlock(&cud->lock);

    RHO_TRACE_EXIT();
}

static struct xutp_conn *
xutp_cud_popconn(struct xutp_cud *cud)
{
    struct xutp_conn *conn = NULL;

    RHO_TRACE_ENTER();

    rhoL_pthread_mutex_lock(&cud->lock);
    while ((RHO_SIMPLEQ_EMPTY(&cud->connq)))
        rhoL_pthread_cond_wait(&cud->cv, &cud->lock);

    conn = RHO_SIMPLEQ_FIRST(&cud->connq);
    RHO_SIMPLEQ_REMOVE_HEAD(&cud->connq, next);
    rhoL_pthread_mutex_unlock(&cud->lock);

    RHO_TRACE_EXIT();
    return (conn);
}

static void
xutp_cudtable_destroy(struct xutp_cudtable *head)
{
    struct xutp_cud *cud = NULL;

    RHO_TRACE_ENTER();

    rhoL_pthread_mutex_lock(&xutp_cudtable_lock);
    while ((cud = RHO_LIST_FIRST(head)) != NULL) {
        RHO_LIST_REMOVE(cud, next);
        xutp_cud_destroy(cud);
    }
    rhoL_pthread_mutex_unlock(&xutp_cudtable_lock);

    RHO_TRACE_EXIT();
}

static void
xutp_cudtable_insert(struct xutp_cud *cud)
{
    RHO_TRACE_ENTER();

    rhoL_pthread_mutex_lock(&xutp_cudtable_lock);
    RHO_LIST_INSERT_HEAD(&xutp_cudtable, cud, next);
    rhoL_pthread_mutex_unlock(&xutp_cudtable_lock);

    RHO_TRACE_EXIT();
}

/* defined but not used */
#if 0
static struct xutp_cud *
xutp_fd2cud(int fd)
{
    bool found = false;
    struct xutp_cud *cud = NULL;

    RHO_TRACE_ENTER();

    rhoL_pthread_mutex_lock(&xutp_cudtable_lock);
    RHO_LIST_FOREACH(cud, &xutp_cudtable, next) {
        if (cud->fd == fd) {
            found = true;
            break;
        }
    }
    rhoL_pthread_mutex_unlock(&xutp_cudtable_lock);

    if (!found)
        cud = NULL;

    RHO_TRACE_EXIT();

    return (cud);
}
#endif

/**********************************************
 * XUTP_SUD (the utp_[s]ocket [u]ser [d]ata)
 * XUTP_SUDTABLE
 ***********************************************/
static struct xutp_sud *
xutp_sud_create(utp_context *ctx)
{
    struct xutp_sud *sud = NULL;

    RHO_TRACE_ENTER();

    sud = rhoL_calloc(1, sizeof(*sud));
    sud->ctx = ctx;

    rhoL_pthread_mutex_init(&sud->lock, NULL);
    rhoL_pthread_cond_init(&sud->cv, NULL);
    sud->blocking = true;
    RHO_SIMPLEQ_INIT(&sud->mbufq);

    RHO_TRACE_EXIT();
    return (sud);
}

static void
xutp_sud_destroy(struct xutp_sud *sud)
{
    RHO_TRACE_ENTER();

    pthread_mutex_destroy(&sud->lock);
    pthread_cond_destroy(&sud->cv);
    xutp_mbufq_destroy(&sud->mbufq);
    free(sud);

    RHO_TRACE_EXIT();
}

static void
xutp_sud_changestate(struct xutp_sud *sud, int state)
{
    RHO_TRACE_ENTER();

    rhoL_pthread_mutex_lock(&sud->lock);  
    sud->state = state;
    rhoL_pthread_cond_signal(&sud->cv);
    rhoL_pthread_mutex_unlock(&sud->lock);  

    RHO_TRACE_EXIT();
}

static void
xutp_sud_pushmbuf(struct xutp_sud *sud, struct xutp_mbuf *mbuf)
{
    rhoL_pthread_mutex_lock(&sud->lock);
    //fprintf(stderr, "adding mbuf\n");
    RHO_SIMPLEQ_INSERT_TAIL(&sud->mbufq, mbuf, next);
    rhoL_pthread_cond_signal(&sud->cv);
    rhoL_pthread_mutex_unlock(&sud->lock);
}

/* TODO: might have to have better error reporting.
 *
 * Currently:
 * -1 on error (e.g., not connected; 0 on timeout, > 0 on succcess 
 */
ssize_t
xutp_sud_awaitrecv(struct xutp_sud *sud, void *buf, size_t len)
{
    struct xutp_mbuf *mbuf = NULL;
    ssize_t nbytes = 0; /* the return value */
    struct timespec ts = { 0 };

    rhoL_pthread_mutex_lock(&sud->lock);
    while ((sud->state == XUTP_STATE_CONNECTED) && RHO_SIMPLEQ_EMPTY(&sud->mbufq)) {
        if (sud->blocking) {
            rhoL_pthread_cond_wait(&sud->cv, &sud->lock);
        } else {
            if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
                rho_errno_die(errno, "clock_gettime failed");
            ts.tv_sec += sud->timeout.tv_sec;
            ts.tv_nsec += sud->timeout.tv_nsec;
            /* return 0 on success, 1 on timed-out */
            if (rhoL_pthread_cond_timedwait(&sud->cv, &sud->lock, &ts) != 0) {
                nbytes = XUTP_ETIMEDOUT;
                goto timeout;
            }
        }
    }

    /* might also allow if remote closed but there are still
     * mbufs in the queue */
    if (sud->state == XUTP_STATE_CONNECTED) {
        mbuf = RHO_SIMPLEQ_FIRST(&sud->mbufq);
        nbytes = MIN(len, (mbuf->buflen - mbuf->off));
        memcpy(buf, mbuf->buf + mbuf->off, nbytes);
        mbuf->off += nbytes;
        if (mbuf->off >= mbuf->buflen) {
            RHO_SIMPLEQ_REMOVE_HEAD(&sud->mbufq, next);
            //fprintf(stderr, "removing mbuf (off=%zu, buflen=%zu)\n", mbuf->off, mbuf->buflen);
            xutp_mbuf_destroy(mbuf);
        }
    } else {
        nbytes = XUTP_ENOTCONN;
    }
    rhoL_pthread_mutex_unlock(&sud->lock);

timeout:
    return (nbytes);
}

static int
xutp_sud_awaitconnected(struct xutp_sud *sud)
{
    int ret = XUTP_ESUCCESS;
    struct timespec ts = { 0 };

    RHO_TRACE_ENTER();

    rhoL_pthread_mutex_lock(&sud->lock);
    while (sud->state != XUTP_STATE_CONNECTED) {
        if (sud->state == XUTP_STATE_ERROR) {
            ret = sud->err;
            goto done;
        }

        if (sud->blocking) {
            rhoL_pthread_cond_wait(&sud->cv, &sud->lock);
        } else {
            if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
                rho_errno_die(errno, "clock_gettime failed");
            ts.tv_sec += sud->timeout.tv_sec;
            ts.tv_nsec += sud->timeout.tv_nsec;
            /* return 0 on success, 1 on timed-out */
            if (rhoL_pthread_cond_timedwait(&sud->cv, &sud->lock, &ts) != 0) {
                ret = XUTP_ETIMEDOUT;
                goto timeout;
            }
        }
    }
    
done:
    rhoL_pthread_mutex_unlock(&sud->lock);
timeout:
    RHO_TRACE_EXIT();
    return (ret);
}

/* defined but not used */
#if 0
static bool
xutp_sud_awaitwritable(struct xutp_sud *sud)
{
    bool ret = false;

    RHO_TRACE_ENTER();

    rhoL_pthread_mutex_lock(&sud->lock);
    while (sud->state != XUTP_STATE_WRITABLE) {
        if (sud->state == XUTP_STATE_ERROR)
            goto done;
        rhoL_pthread_cond_wait(&sud->cv, &sud->lock);
    }
    ret = true;

done:
    rhoL_pthread_mutex_unlock(&sud->lock);
    RHO_TRACE_EXIT();
    return (ret);
}
#endif

static void
xutp_sudtable_destroy(struct xutp_sudtable *head)
{
    struct xutp_sud *sud = NULL;

    RHO_TRACE_ENTER();

    while ((sud = RHO_LIST_FIRST(head)) != NULL) {
        RHO_LIST_REMOVE(sud, next);
        xutp_sud_destroy(sud);
    }

    RHO_TRACE_EXIT();
}

/* XXX: coupled, since also sets the xfd on the sud */
static int
xutp_sudtable_insert(struct xutp_sud *sud)
{
    RHO_TRACE_ENTER();

    rhoL_pthread_mutex_lock(&xutp_sudtable_lock);
    xutp_xfdcnt++;
    sud->xfd = xutp_xfdcnt;
    RHO_LIST_INSERT_HEAD(&xutp_sudtable, sud, next);
    rhoL_pthread_mutex_unlock(&xutp_sudtable_lock);

    RHO_TRACE_EXIT();
    return (sud->xfd);
}

static struct xutp_sud *
xutp_xfd2sud(int xfd)
{
    bool found = false;
    struct xutp_sud *sud = NULL;

    RHO_TRACE_ENTER();

    rhoL_pthread_mutex_lock(&xutp_sudtable_lock);
    RHO_LIST_FOREACH(sud, &xutp_sudtable, next) {
        if (sud->xfd == xfd) {
            found = true;
            break;
        }
    }
    rhoL_pthread_mutex_unlock(&xutp_sudtable_lock);

    if (!found)
        sud = NULL;

    RHO_TRACE_EXIT();

    return (sud);
}

/**********************************************
 * XUTP_POLLARRAY
 ***********************************************/
static struct xutp_pollarray *
xutp_pollarray_create(void)
{
    struct xutp_pollarray *pa = NULL;

    RHO_TRACE_ENTER();

    pa = rhoL_calloc(1, sizeof(*pa));
    pa->capacity = XUTP_POLLARRAY_INITCAPACITY;
    pa->pollfds = rhoL_calloc(pa->capacity, sizeof(struct pollfd)); 
    pa->cuds = rhoL_calloc(pa->capacity, sizeof(struct xutp_cud *));

    RHO_TRACE_EXIT();
    return (pa);
}

static void
xutp_pollarray_destroy(struct xutp_pollarray *pa)
{
    RHO_TRACE_ENTER();

    free(pa->pollfds);
    free(pa->cuds);
    free(pa);

    RHO_TRACE_EXIT();
}

static void
xutp_pollarray_push(struct xutp_pollarray *pa, struct xutp_cud *cud,
        short events)
{
    //RHO_TRACE_ENTER();

    if (pa->nfds == pa->capacity) {
        /* TODO: check for overflow */
        pa->capacity *= 2;
        pa->pollfds = rhoL_realloc(pa->pollfds,
                sizeof(struct pollfd) * pa->capacity);
        pa->cuds = rhoL_realloc(pa->cuds, sizeof(struct xutp_cud *) * pa->capacity);
    }

    pa->pollfds[pa->nfds].fd = cud->fd; 
    pa->pollfds[pa->nfds].events = events;
    pa->cuds[pa->nfds] = cud;

    pa->nfds++;

    //RHO_TRACE_EXIT();
}

static void
xutp_pollarray_clear(struct xutp_pollarray *pa)
{
    //RHO_TRACE_ENTER();

    pa->nfds = 0;

    //RHO_TRACE_EXIT();
}

static void
xutp_pollarray_refresh(struct xutp_pollarray *pa)
{
    struct xutp_cud *cud = NULL;
    struct xutp_cud *tmp = NULL;

    RHO_TRACE_ENTER();

    xutp_pollarray_clear(pa);
    rhoL_pthread_mutex_lock(&xutp_cudtable_lock);

        /* TODO: only add cuds that are ACTIVE_CLIENT or ACTIVE_SERVER */
        /* TODO: garbage collection -- if refcnt of any cud's are 0, call
         * utp_destroy(cud->ctx)
         */
    RHO_LIST_FOREACH_SAFE(cud, &xutp_cudtable, next, tmp) {
        if (cud->refcnt <= 0) {
            RHO_LIST_REMOVE(cud, next);
            utp_destroy(cud->ctx);
            xutp_cud_destroy(cud);
        } else {
            xutp_pollarray_push(pa, cud, POLLIN);
        }
    }
    rhoL_pthread_mutex_unlock(&xutp_cudtable_lock);

    RHO_TRACE_EXIT();
}

/**********************************************
 * LIBUTP CALLBACKS
 ***********************************************/

/* valid 'a' fields
 *  - context
 *  - socket
 *  - buf (probably a null-terminated string)
 */
static uint64
xutp_log(utp_callback_arguments *a)
{
    /* TODO: enable optional logging */
    //fprintf(stderr, "[log] %s\n", a->buf);

    return (0);
}

/* valid 'a' fields
 *   - context
 *   - socket
 *   - error_code
 *      { UTP_ETIMEDOUT, ECONNRESET, ECONNREFUSED }
 *
 * TODO: what state is the socket in when this callback is called?
 */
static uint64
xutp_on_error(utp_callback_arguments *a)
{
    struct xutp_sud *sud = NULL;

    RHO_TRACE_ENTER();

    sud = utp_get_userdata(a->socket);
    sud->state = XUTP_STATE_ERROR;

    switch (a->error_code) {
    case UTP_ETIMEDOUT:
        sud->err = XUTP_ETIMEDOUT;
        break;
    case UTP_ECONNRESET:
        sud->err = XUTP_ECONNRESET;
        break;
    case UTP_ECONNREFUSED:
        sud->err = XUTP_ECONNREFUSED;
        break;
    default:
        sud->err = XUTP_EUNKNOWN;
    }
    
    /* TODO: need to signal condvar */

    RHO_TRACE_EXIT();
    return (0);
}

/* valid 'a' fields
 *   - ctx
 *   - address
 *   - address_len
 *
 * return 0 for allow incoming 'connection' from address;
 * return non-zero for deny
 */
static uint64
xutp_on_firewall(utp_callback_arguments *a)
{
    uint64 deny = 0;
    struct xutp_cud *cud = NULL;

    RHO_TRACE_ENTER();

    cud = utp_context_get_userdata(a->context);
    if (cud->type == XUTP_CUD_TYPE_ACTIVE_SERVER)
        deny = 1; 

    RHO_TRACE_EXIT();
    return (deny);
}

/* valid 'a' fields
 *   - context
 *   - socket
 *   - state
 *
 * UTP_STATE_DESTROYING should only be called from net-thread (invoked via
 * utp_check_timeouts).
 */
static uint64
xutp_on_state_change(utp_callback_arguments *a)
{
    struct xutp_sud *sud = NULL;
    int state = 0;

    RHO_TRACE_ENTER();

    state = a->state;
    
    if (state != UTP_STATE_CONNECT && state != UTP_STATE_WRITABLE &&
            state != UTP_STATE_EOF && state != UTP_STATE_DESTROYING) {
        rho_die("unknown state: %d\n", a->state);
    }

    sud = utp_get_userdata(a->socket);


    switch (a->state) {
    case UTP_STATE_WRITABLE:
        /* socket is able to send more data */
        //fprintf(stderr, "state writable\n");
        xutp_sud_changestate(sud, XUTP_STATE_WRITABLE);
        break;
    case UTP_STATE_EOF:
        /* connection closed */
        //fprintf(stderr, "state eof\n");
        xutp_sud_changestate(sud, XUTP_STATE_CLOSED);
        break;
    case UTP_STATE_DESTROYING:
        //fprintf(stderr, "state destroying\n");
        RHO_LIST_REMOVE(sud, next);
        xutp_sud_destroy(sud);
        break;

    default:
        /* can't get here */
        assert(0);
        break;
    }

    RHO_TRACE_EXIT();
    return (0);
}

/* valid 'a' fields
 *   - context
 *   - socket ( .state should be CS_CONNECTED)
 *
 * socket
 */
static uint64
xutp_on_connect(utp_callback_arguments *a)
{
    struct xutp_sud *sud = NULL;

    RHO_TRACE_ENTER();

    sud = utp_get_userdata(a->socket);
    xutp_sud_changestate(sud, XUTP_STATE_CONNECTED);

    RHO_TRACE_EXIT();
    XUTP_RETURN_IGNORED(0);
}

/* valid 'a' fields:
 *   - ctx
 *   - socket   (newly created)
 *   - address
 *   - address_len
 *
 * utp_process_udp
 *   utp_create_socket
 *   utp_initialize_socket
 *   conn->state = CS_SYN_RECV
 *   utp_process_incoming
 *   conn->send_ack(true)
 *      send_data
 *        send_to_addr
 *          utpp_register_sent_packet
 *          utp_call_sendto
 *   utp_call_on_accept
 *
 * The new utp_socket is created just before this call
 */
static uint64
xutp_on_accept(utp_callback_arguments *a)
{
    struct xutp_cud *cud = NULL;
    struct xutp_sud *sud = NULL;
    struct xutp_conn *conn = NULL;
    int xfd = 0;

    RHO_TRACE_ENTER();  

    cud = utp_context_get_userdata(a->context);
    cud->refcnt++;

    sud = xutp_sud_create(a->context);
    sud->sk = a->socket;
    utp_set_userdata(a->socket, sud);
    xfd = xutp_sudtable_insert(sud);

    conn = xutp_conn_create(xfd);
    xutp_cud_pushconn(cud, conn);
    
    RHO_TRACE_EXIT();  
    XUTP_RETURN_IGNORED(0);
}

/* valid 'a' fields:
 *   - context
 *   - socket
 *   - buf (const byte)
 *   - len  (size_t)
 *
 * XXX: move data to the socket's or context's userdata buffer?
 */
static uint64
xutp_on_read(utp_callback_arguments *a)
{
    struct xutp_sud *sud = NULL;
    struct xutp_mbuf *mbuf = NULL;

    RHO_TRACE_ENTER();  

    //fprintf(stderr, "************* got %lu bytes\n", (unsigned long)a->len);

    sud = utp_get_userdata(a->socket); 
    mbuf = xutp_mbuf_create(a->buf, a->len);
    xutp_sud_pushmbuf(sud, mbuf);

    utp_read_drained(a->socket);

    RHO_TRACE_EXIT();  
    XUTP_RETURN_IGNORED(0);
}

/* valid 'a' fields
 *   - context
 *   - socket   (I believe this is actualy NULL)
 *   - buf
 *   - len
 *   - flags
 *   - address
 *   - address_len
 *
 * just call sendto() to sed the data out
 */
static uint64
xutp_on_sendto(utp_callback_arguments *a)
{
    struct xutp_cud *cud = NULL;

    RHO_TRACE_ENTER();  
    
    cud = utp_context_get_userdata(a->context);
    rhoL_sendto(cud->fd, a->buf, a->len, 0, a->address, a->address_len);

    RHO_TRACE_EXIT();  
    XUTP_RETURN_IGNORED(0);
}


/**********************************************
 * XUTP NETWORK THREAD
 ***********************************************/ 
static void *
xutp_netloop(void *arg)
{
    int n = 0;
    unsigned int i = 0;
    struct xutp_pollarray *pa = NULL;

    RHO_TRACE_ENTER();  

    (void)arg;

    pa = xutp_pollarray_create();

    xutp_netloop_started = true;
    while (!xutp_fini_called) {
        xutp_pollarray_refresh(pa);

        n = poll(pa->pollfds, pa->nfds, XUTP_POLL_TIMEOUT);

        if (n == -1) {
            rho_errno_die(errno, "poll");
        } else if (n > 0) {
            for (i = 0; i < pa->nfds; i++) {
                if (pa->pollfds[i].revents & POLLERR) {
                    rho_warn("POLLERR");
                }
                if ((pa->pollfds[i].revents & POLLIN)) {
                    //fprintf(stderr, "POLLIN for cud %d (fd=%d)\n", i, pa->cuds[i]->fd);
                    xutp_cud_recv(pa->cuds[i]);
                }
            } 
        } else {
            ; 
            //fprintf(stderr, "poll timeout\n");
        }

        xutp_pollarray_refresh(pa);
        for (i = 0; i < pa->nfds; i++) {
	        utp_check_timeouts(pa->cuds[i]->ctx);
        }
    }

    xutp_pollarray_destroy(pa);

    RHO_TRACE_EXIT();  
    return (NULL);
}

/**********************************************************
 *
 * XUTP PUBLIC API
 *
 *********************************************************/

/**********************************************************
 * STARTUP/SHUTDOWN
 *********************************************************/
void
xutp_init(void)
{
    RHO_TRACE_ENTER();  

    /* TODO: set any context options */
    rhoL_pthread_create(&xutp_netthread, NULL, xutp_netloop, NULL);
    /* wait for net-thread setup; for nowp */
    while (xutp_netloop_started == false) ;

    RHO_TRACE_EXIT();  
}

void
xutp_fini(void)
{
    int error = 0;
    RHO_TRACE_ENTER();  

    xutp_fini_called = true;
    error = pthread_join(xutp_netthread, NULL);
    if (error != 0)
        rho_errno_warn(error, "pthread_join failed");
    xutp_cudtable_destroy(&xutp_cudtable);
    xutp_sudtable_destroy(&xutp_sudtable);

    RHO_TRACE_EXIT();  
}

/**********************************************************
 * GLUE
 *********************************************************/
int 
xutp_newsock(const struct sockaddr *addr, socklen_t alen)
{

    int fd = 0;
    utp_context *ctx = NULL;
    struct xutp_cud *cud = NULL;
    struct xutp_sud *sud = NULL;
    int xfd = 0;

    RHO_TRACE_ENTER();

    fd = rhoL_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (addr != NULL) {
        rhoL_bind(fd, addr, alen);
        /* TODO: set laddr, lalen */
    }

    ctx = utp_init(2); 
    if (ctx == NULL)
        rho_die("utp_init");

    utp_set_callback(ctx, UTP_LOG, xutp_log);
    utp_set_callback(ctx, UTP_ON_ERROR, xutp_on_error);
    utp_set_callback(ctx, UTP_ON_FIREWALL, xutp_on_firewall);
    utp_set_callback(ctx, UTP_ON_STATE_CHANGE, xutp_on_state_change);
    utp_set_callback(ctx, UTP_ON_ACCEPT, xutp_on_accept);
    utp_set_callback(ctx, UTP_ON_CONNECT, xutp_on_connect);
    utp_set_callback(ctx, UTP_ON_READ, xutp_on_read);
    utp_set_callback(ctx, UTP_SENDTO, xutp_on_sendto);

    /* TODO: toggablable */
    //utp_context_set_option(ctx, UTP_LOG_NORMAL, 1);
    //utp_context_set_option(ctx, UTP_LOG_MTU, 1);
    //utp_context_set_option(ctx, UTP_LOG_DEBUG, 1);

    cud = xutp_cud_create(ctx, fd);
    xutp_cudtable_insert(cud);

    sud = xutp_sud_create(ctx);
    xfd = xutp_sudtable_insert(sud);

    RHO_TRACE_EXIT();
    return (xfd);
}

/**********************************************************
 * BERKELEY-ESQUE SOCKET API
 *********************************************************/
int
xutp_connect(int xfd, struct sockaddr *addr, socklen_t alen)
{
    int ret = XUTP_ESUCCESS;
    utp_socket *sk = NULL;
    struct xutp_cud *cud = NULL;
    struct xutp_sud *sud = NULL;

    RHO_TRACE_ENTER();

    sud = xutp_xfd2sud(xfd);
    if (sud == NULL) {
        ret = XUTP_EBADF;
        goto done;
    }

    if (sud->sk != NULL) {
        ret = XUTP_EISCONN;
        goto done;
    }

    cud = utp_context_get_userdata(sud->ctx);
    cud->type = XUTP_CUD_TYPE_ACTIVE_CLIENT;

    sk = utp_create_socket(sud->ctx);
    sud->sk = sk;
    utp_set_userdata(sk, sud);

    utp_connect(sk, addr, alen);
    ret = xutp_sud_awaitconnected(sud);

done:
    RHO_TRACE_EXIT();
    return (ret);
}

int
xutp_listen(int xfd, int backlog)
{
    int ret = XUTP_ESUCCESS;
    struct xutp_cud *cud = NULL;
    struct xutp_sud *sud = NULL;

    RHO_TRACE_ENTER();

    (void)backlog;

    sud = xutp_xfd2sud(xfd);
    if (sud == NULL) {
        ret = XUTP_EBADF;
        goto done;
    }

    if (sud->sk != NULL) {
        ret = XUTP_EISCONN;
        goto done;
    }

    cud = utp_context_get_userdata(sud->ctx);
    cud->type = XUTP_CUD_TYPE_PASSIVE_SERVER;

done:
    RHO_TRACE_EXIT();
    return (ret);
}

int
xutp_accept(int xfd, struct sockaddr *addr, socklen_t *alen)
{
    int newxfd = 0;
    struct xutp_cud *cud = NULL;
    struct xutp_sud *sud = NULL;
    struct xutp_conn *conn = NULL;

    RHO_TRACE_ENTER();

    /* TODO */
    (void)addr;
    (void)alen;

    sud = xutp_xfd2sud(xfd);
    if (sud == NULL) {
        /* TODO: need an errno-liek varible; set to XUTP_EBADF */
        newxfd = -1;
        goto done;
    }

    if (sud->sk != NULL) {
        /* TODO: need an errno-like varible; set to XUTP_EISCONN */
        newxfd = -1;
        goto done;
    }

    cud = utp_context_get_userdata(sud->ctx);
    conn = xutp_cud_popconn(cud);
    newxfd = conn->xfd;
    xutp_conn_destroy(conn);

    /* TODO: populate addr and alen out parameters */

done:
    RHO_TRACE_EXIT();
    return (newxfd);
}

ssize_t
xutp_send(int xfd, const void *buf, size_t len)
{
    ssize_t ret = 0;
    struct xutp_sud *sud = NULL;
    size_t n = 0;

    RHO_TRACE_ENTER();

    sud = xutp_xfd2sud(xfd);
    if (sud == NULL) {
        ret = XUTP_EBADF;
        goto done;
    }

    if (sud->sk == NULL) {
        ret = XUTP_ENOTCONN;
        goto done; 
    }

    n = utp_write(sud->sk, (void *)buf, len);
    /* XXX: unsafe cast */
    ret = ((ssize_t) n);

done:
    RHO_TRACE_EXIT();
    return (ret);
}

ssize_t
xutp_recv(int xfd, void *buf, size_t len)
{
    ssize_t ret = 0;
    struct xutp_sud *sud = NULL;

    RHO_TRACE_ENTER();

    sud = xutp_xfd2sud(xfd);
    
    if (sud == NULL) {
        ret = XUTP_EBADF;
        goto done;
    }

    if (sud->sk == NULL) {
        ret = XUTP_ENOTCONN;
        goto done; 
    }

    ret = xutp_sud_awaitrecv(sud, buf, len);

done:
    RHO_TRACE_EXIT();
    return (ret);
}

int
xutp_close(int xfd)
{
    int ret = XUTP_ESUCCESS;
    struct xutp_cud *cud = NULL;
    struct xutp_sud *sud = NULL;

    RHO_TRACE_ENTER();

    sud = xutp_xfd2sud(xfd);
    if (sud == NULL) {
        ret = XUTP_EBADF;
        goto done;
    }

    cud = utp_context_get_userdata(sud->ctx);
    cud->refcnt--;

    /* if an active client socket */
    switch (cud->type) {
    case XUTP_CUD_TYPE_ACTIVE_CLIENT:
    case XUTP_CUD_TYPE_ACTIVE_SERVER:
        utp_close(sud->sk);
        break;
    case XUTP_CUD_TYPE_PASSIVE_SERVER:
        assert(sud->sk == NULL);
        if (cud->refcnt > 0)
            cud->type = XUTP_CUD_TYPE_PASSIVE_SERVER;
        break;
    default:
        assert(0);
    }

    /* XXX: wait? */

done:
    RHO_TRACE_EXIT();
    return (ret);
}

int
xutp_setnonblocking(int xfd)
{
    int ret = XUTP_ESUCCESS;
    struct xutp_sud *sud = NULL;

    RHO_TRACE_ENTER();

    sud = xutp_xfd2sud(xfd);
    if (sud == NULL) {
        ret = XUTP_EBADF;
        goto done;
    }

    sud->blocking = false;
    sud->timeout.tv_sec = 0;
    sud->timeout.tv_nsec = 10000000;  /* tenth of a second */

done:
    RHO_TRACE_EXIT();
    return (ret);
}

int
xutp_setblocking(int xfd)
{
    int ret = XUTP_ESUCCESS;
    struct xutp_sud *sud = NULL;

    RHO_TRACE_ENTER();

    sud = xutp_xfd2sud(xfd);
    if (sud == NULL) {
        ret = XUTP_EBADF;
        goto done;
    }

    sud->blocking = true;

done:
    RHO_TRACE_EXIT();
    return (ret);
} 

int
xutp_settimeout(int xfd, struct timespec *ts)
{
    int ret = XUTP_ESUCCESS;
    struct xutp_sud *sud = NULL;

    RHO_TRACE_ENTER();

    sud = xutp_xfd2sud(xfd);
    if (sud == NULL) {
        rho_warn("%d is not a valid utp socket descriptor\n", xfd);
        ret = XUTP_EBADF;
        goto done;
    }

    sud->blocking = false;
    sud->timeout = *ts;

done:
    RHO_TRACE_EXIT();
    return (ret);
}

/**********************************************
 * TODO - UNIMPLEMENTED
 ***********************************************/
#if 0
/* return 0 on success; -1 on failure */
int
xutp_getpeername(int fd, struct sockaddr *addr, socklen_t *alen)
{
    int ret = 0;
    utp_socket *sk = NULL;
    struct xutp_sud *sud = NULL;

    RHO_TRACE_ENTER();  
    
    sud = xutp_fd2sud(fd);

    if (sud->type == XUTP_TYPE_SERVERPASSIVE) {
        ret = -1;
        goto done;
    }

    sk = sud->sk;
    ret = utp_getpeername(sk, addr, alen);

done:
    RHO_TRACE_EXIT();  
    return (ret);
}

static void
xutp_getsockname(int fd, struct sockaddr *addr, socklen_t *alen)
{
    struct xutp_sud *sud = NULL;

    RHO_TRACE_ENTER();  

    sud = xutp_fd2sud(fd);
    memcpy(addr, &sud->laddr, MIN(*alen, sud->lalen));
    *alen = sud->lalen;

    RHO_TRACE_EXIT();
} 
#endif
