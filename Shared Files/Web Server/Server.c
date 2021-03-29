/* 
   very *cough* secure C99 web server with MIT code originally written by 
   github/shenfeng then extended by @cwkx to add sqlite3 and GET handling
   to compile and run, type: tcc Server.c -lsqlite3 -o Server && ./Server
*/

#include <arpa/inet.h> /* inet_ntoa */
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LISTENQ 1024 /* second argument to listen() */
#define MAXLINE 1024 /* max length of a line */
#define RIO_BUFSIZE 1024

typedef struct {
	int fd;  /* descriptor */
	int row; /* row ID */
	int tr;  /* transpose */
} sql_fd;

typedef struct {
    int rio_fd;                 /* descriptor for this buf */
    int rio_cnt;                /* unread byte in this buf */
    char *rio_bufptr;           /* next unread byte in this buf */
    char rio_buf[RIO_BUFSIZE];  /* internal buffer */
} rio_t;

/* simplifies calls to bind(), connect(), and accept() */
typedef struct sockaddr SA;

typedef struct {
    char filename[512];
    char content[512];	/* for file?content... */
    off_t offset; /* for support Range */
    size_t end;
} http_request;

typedef struct {
    const char *extension;
    const char *mime_type;
} mime_map;

mime_map meme_types [] = {
    {".css", "text/css"},
    {".gif", "image/gif"},
    {".htm", "text/html"},
    {".html", "text/html"},
    {".jpeg", "image/jpeg"},
    {".jpg", "image/jpeg"},
    {".ico", "image/x-icon"},
    {".js", "application/javascript"},
    {".pdf", "application/pdf"},
    {".mp4", "video/mp4"},
    {".png", "image/png"},
    {".svg", "image/svg+xml"},
    {".xml", "text/xml"},
    {NULL, NULL},
};

char *default_mime_type = "text/plain";

void rio_readinitb(rio_t *rp, int fd)
{
    rp->rio_fd = fd;
    rp->rio_cnt = 0;
    rp->rio_bufptr = rp->rio_buf;
}

ssize_t writen(int fd, void *usrbuf, size_t n)
{
    size_t nleft = n;
    ssize_t nwritten;
    char *bufp = usrbuf;

    while (nleft > 0){
        if ((nwritten = write(fd, bufp, nleft)) <= 0){
            if (errno == EINTR)  /* interrupted by sig handler return */
                nwritten = 0;    /* and call write() again */
            else
                return -1;       /* errorno set by write() */
        }
        nleft -= nwritten;
        bufp += nwritten;
    }
    return n;
}

/* sqlite3 callback called once per row */
int sql_callback_write_table(void *ptr, int argc, char **argv, char **az_col_name)
{
    sql_fd* sf = (sql_fd*)ptr;
    int fd = sf->fd;
    
    char buf[MAXLINE];
    char* cls = sf->tr ? "transpose" : "";
    
	/* column header for first row */
    if (argc > 0) {
    	if (sf->row == 0) {
    		sprintf(buf, "<tr class='%s'>", cls);
    		writen(fd, buf, strlen(buf));
    		
    		for (int i=0; i<argc; ++i) {
    			sprintf(buf, "<th class='%s'>%s</th>", cls, az_col_name[i]);
    		    writen(fd, buf, strlen(buf));
    		}
    		
    		sprintf(buf, "%s", "</tr>");
    		writen(fd, buf, strlen(buf));
    	}
    }
    
    sprintf(buf, "<tr class='%s'>", cls);
    writen(fd, buf, strlen(buf));
    
    /* each item in row */
    for (int i = 0; i < argc; i++) {
        sprintf(buf, "<td class='%s'>%s</td>", cls, argv[i] ? argv[i] : "NULL");
        writen(fd, buf, strlen(buf));
    }
    
    /* footer */
    sprintf(buf, "%s", "</tr>");
    writen(fd, buf, strlen(buf));

	/* increment row */
	sf->row = sf->row+1;

    return 0;
}

/*
 * rio_read - This is a wrapper for the Unix read() function that
 * transfers min(n, rio_cnt) bytes from an internal buffer to a user
 * buffer, where n is the number of bytes requested by the user and
 * rio_cnt is the number of unread bytes in the internal buffer. On
 * entry, rio_read() refills the internal buffer via a call to
 * read() if the internal buffer is empty.
 */
/* $begin rio_read */
static ssize_t rio_read(rio_t *rp, char *usrbuf, size_t n)
{
    int cnt;
    while (rp->rio_cnt <= 0){ /* refill if buf is empty */

        rp->rio_cnt = read(rp->rio_fd, rp->rio_buf,
                           sizeof(rp->rio_buf));
        if (rp->rio_cnt < 0){
            if (errno != EINTR) /* interrupted by sig handler return */
                return -1;
        }
        else if (rp->rio_cnt == 0) /* EOF */
            return 0;
        else
            rp->rio_bufptr = rp->rio_buf; /* reset buffer ptr */
    }

    /* copy min(n, rp->rio_cnt) bytes from internal buf to user buf */
    cnt = n;
    if (rp->rio_cnt < n)
        cnt = rp->rio_cnt;
    memcpy(usrbuf, rp->rio_bufptr, cnt);
    rp->rio_bufptr += cnt;
    rp->rio_cnt -= cnt;
    return cnt;
}

/* rio_readlineb - robustly read a text line (buffered) */
ssize_t rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen)
{
    int n, rc;
    char c, *bufp = usrbuf;

    for (n = 1; n < maxlen; n++){
        if ((rc = rio_read(rp, &c, 1)) == 1){
            *bufp++ = c;
            if (c == '\n')
                break;
        } else if (rc == 0){
            if (n == 1)
                return 0; /* EOF, no data read */
            else
                break; /* EOF, some data was read */
        } else
            return -1; /* error */
    }
    *bufp = 0;
    return n;
}

void format_size(char* buf, struct stat *stat)
{
    if(S_ISDIR(stat->st_mode)){
        sprintf(buf, "%s", "[DIR]");
    } else {
        off_t size = stat->st_size;
        if(size < 1024){
            sprintf(buf, "%lu", size);
        } else if (size < 1024 * 1024){
            sprintf(buf, "%.1fK", (double)size / 1024);
        } else if (size < 1024 * 1024 * 1024){
            sprintf(buf, "%.1fM", (double)size / 1024 / 1024);
        } else {
            sprintf(buf, "%.1fG", (double)size / 1024 / 1024 / 1024);
        }
    }
}

void handle_directory_request(int out_fd, int dir_fd, char *filename)
{
    char buf[MAXLINE], m_time[32], size[16];
    struct stat statbuf;
    sprintf(buf, "HTTP/1.1 200 OK\r\n%s%s%s%s%s",
            "Content-Type: text/html\r\n\r\n",
            "<html><head><style>",
            "body{font-family: monospace; font-size: 13px;}",
            "td {padding: 1.5px 6px;}",
            "</style></head><body><table>\n");
    writen(out_fd, buf, strlen(buf));
    DIR *d = fdopendir(dir_fd);
    struct dirent *dp;
    int ffd;
    while ((dp = readdir(d)) != NULL){
        if(!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")){
            continue;
        }
        if ((ffd = openat(dir_fd, dp->d_name, O_RDONLY)) == -1){
            perror(dp->d_name);
            continue;
        }
        fstat(ffd, &statbuf);
        strftime(m_time, sizeof(m_time),
                 "%Y-%m-%d %H:%M", localtime(&statbuf.st_mtime));
        format_size(size, &statbuf);
        if(S_ISREG(statbuf.st_mode) || S_ISDIR(statbuf.st_mode)){
            char *d = S_ISDIR(statbuf.st_mode) ? "/" : "";
            sprintf(buf, "<tr><td><a href=\"%s%s\">%s%s</a></td><td>%s</td><td>%s</td></tr>\n",
                    dp->d_name, d, dp->d_name, d, m_time, size);
            writen(out_fd, buf, strlen(buf));
        }
        close(ffd);
    }
    sprintf(buf, "</table></body></html>");
    writen(out_fd, buf, strlen(buf));
    closedir(d);
}

static const char* get_mime_type(char *filename){
    char *dot = strrchr(filename, '.');
    if(dot){ /* strrchar Locate last occurrence of character in string */
        mime_map *map = meme_types;
        while(map->extension){
            if(strcmp(map->extension, dot) == 0){
                return map->mime_type;
            }
            map++;
        }
    }
    return default_mime_type;
}

int open_listenfd(int port)
{
    int listenfd, optval=1;
    struct sockaddr_in serveraddr;

    /* create a socket descriptor */
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;

    /* eliminates "Address already in use" error from bind. */
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int)) < 0)
        return -1;

    /* 6 is TCP's protocol number enable this, much faster : 4000 req/s -> 17000 req/s */
    if (setsockopt(listenfd, 6, TCP_CORK, (const void *)&optval , sizeof(int)) < 0)
        return -1;

    /* listenfd will be an endpoint for all requests to port on any IP address for this host */
    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)port);
    if (bind(listenfd, (SA *)&serveraddr, sizeof(serveraddr)) < 0)
        return -1;

    /* make it a listening socket ready to accept connection requests */
    if (listen(listenfd, LISTENQ) < 0)
        return -1;

    return listenfd;
}

void url_decode(char* src, char* dest, int max)
{
    char *p = src;
    char code[3] = { 0 };
    while(*p && --max) {
        if(*p == '%') {
            memcpy(code, ++p, 2);
            *dest++ = (char)strtoul(code, NULL, 16);
            p += 2;
        } else {
            *dest++ = *p++;
        }
    }
    *dest = '\0';
}

void parse_request(int fd, http_request *req)
{
    rio_t rio;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE];
    req->offset = 0;
    req->end = 0; /* default */

    rio_readinitb(&rio, fd);
    rio_readlineb(&rio, buf, MAXLINE);
    
    sscanf(buf, "%s %s", method, uri); /* version is not cared */
    /* read all */
    while(buf[0] != '\n' && buf[1] != '\n') { /* \n || \r\n */
        rio_readlineb(&rio, buf, MAXLINE);
        if(buf[0] == 'R' && buf[1] == 'a' && buf[2] == 'n'){
            sscanf(buf, "Range: bytes=%lu-%lu", &req->offset, &req->end);
            /* range: [start, end] */
            if( req->end != 0) req->end ++;
        }
    }
    
    /* clean */
	int i=0;
	while (req->content[i] != '\0') {
		req->content[i] = '\0';
		++i;
    }
	char* const sep_at = strchr(uri, '?');
	if(sep_at != NULL)
	{
		*sep_at = '\0';
		/* printf("first part: '%s'\nsecond part: '%s'\n", uri, sep_at + 1); */
		url_decode(sep_at+1, req->content, MAXLINE);
	}
    
    char* filename = uri;
    if(uri[0] == '/')
    {
        filename = uri + 1;
        int length = strlen(filename);
        if (length == 0){
            filename = ".";
        } else {
            for (int i=0; i<length; ++i) {
                if (filename[i] == '?') {
                    filename[i] = '\0';
                    break;
                }
            }
        }
    }
    url_decode(filename, req->filename, MAXLINE);
}

void log_access(int status, struct sockaddr_in *c_addr, http_request *req){
    printf("%s:%d %d - %s\n", inet_ntoa(c_addr->sin_addr),
           ntohs(c_addr->sin_port), status, req->filename);
}

void client_error(int fd, int status, char *msg, char *longmsg){
    char buf[MAXLINE];
    sprintf(buf, "HTTP/1.1 %d %s\r\n", status, msg);
    sprintf(buf + strlen(buf),
            "Content-length: %lu\r\n\r\n", strlen(longmsg));
    sprintf(buf + strlen(buf), "%s", longmsg);
    writen(fd, buf, strlen(buf));
}

void serve_static(int out_fd, int in_fd, http_request *req, size_t total_size)
{
    char buf[256];
    if (req->offset > 0){
        sprintf(buf, "HTTP/1.1 206 Partial\r\n");
        sprintf(buf + strlen(buf), "Content-Range: bytes %lu-%lu/%lu\r\n",
                req->offset, req->end, total_size);
    } else {
        sprintf(buf, "HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\n");
    }
    sprintf(buf + strlen(buf), "Cache-Control: no-cache\r\n");
    sprintf(buf + strlen(buf), "Content-length: %lu\r\n",
            req->end - req->offset);
    sprintf(buf + strlen(buf), "Content-type: %s\r\n\r\n",
            get_mime_type(req->filename));

    writen(out_fd, buf, strlen(buf));
    off_t offset = req->offset; /* copy */
    while(offset < req->end)
    {
        if(sendfile(out_fd, in_fd, &offset, req->end - req->offset) <= 0) {
            break;
        }
        printf("offset: %d \n\n", offset);
        close(out_fd);
        break;
    }
}

void write_table(int fd, sqlite3 *db, const char *sql, int transpose)
{
    /* header */
    sql_fd sf;
    sf.fd = fd;
    sf.row = 0;
    sf.tr = transpose;
    
    char buf[MAXLINE];
    sprintf(buf, "HTTP/1.1 200 OK\r\n%s%s",
            "Content-Type: text/html\r\n\r\n",
            "<table>");
    writen(fd, buf, strlen(buf));
    
    /* rows */
    char *err_msg = 0;
    int rc = sqlite3_exec(db, sql, sql_callback_write_table, &sf, &err_msg);
        
    if (rc != SQLITE_OK ) {
        fprintf(stderr, "Failed to select data\n");
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return;
    }
    
    if (sf.row == 0) {
        sprintf(buf, "No information found");
        writen(fd, buf, strlen(buf));
    }
        
    /* footer */
    sprintf(buf, "%s", "</table>");
    writen(fd, buf, strlen(buf));
    return;
}

void process(int fd, struct sockaddr_in *clientaddr, sqlite3 *db)
{
    printf("accept request, fd is %d, pid is %d\n", fd, getpid());
    http_request req;
    parse_request(fd, &req);
    
    printf("req.filename: %s\n", req.filename);
    /* handle GET requests in a naive way */
    if (strcmp(req.filename, "Files/message") == 0)
    {
    	printf("content: %s\n", req.content);

        char* const sep_at = strchr(req.content, '&');
        if(sep_at != NULL)
            *sep_at = '\0';
        
        char *err_msg = 0;
        char* const name = req.content+5;
        char* const message = sep_at+9;

    	char sql[MAXLINE];
        sprintf(sql, "INSERT INTO Messages VALUES(NULL, '%s', '%s');", name, message);
    	
        if (sqlite3_exec(db, sql, 0, 0, &err_msg) != SQLITE_OK)
        {
            fprintf(stderr, "Failed to create table\n");
            fprintf(stderr, "SQL error: %s\n", err_msg);
            sqlite3_free(err_msg);    
        }
        
        printf("name: %s\nmessage: %s\n", name, message);
    	
        write_table(fd, db, "SELECT Name,Message FROM Messages", 0);
    	return;
    }
    else if (strcmp(req.filename, "Files/refresh") == 0)
    {
    	write_table(fd, db, "SELECT Name,Message FROM Messages", 0);
    	return;
    }
    /* login */
    else if (strcmp(req.filename, "Files/login") == 0)
    {
    	printf("content: %s\n", req.content);

        char* const sep_at = strchr(req.content, '&');
        if(sep_at != NULL)
            *sep_at = '\0';
            
        char* const username = req.content+9;
        char* const password = sep_at+10;
        
        printf("username: %s\npassword: %s\n", username, password);
    	
    	char query[MAXLINE];
        sprintf(query, "SELECT * FROM Users WHERE Users.Login='%s' AND Users.Password='%s'", username, password);
    	
        write_table(fd, db, query, 0);
    	return;
    }
    else if (strcmp(req.filename, "Files/users") == 0)
    {
    	write_table(fd, db, "SELECT Name `Names:` FROM Users", 1);
    	return;
    }
    else if (strcmp(req.filename, "Files/countries") == 0)
    {
    	write_table(fd, db, "SELECT Country `User Countries` FROM Users", 0);
    	return;
    }
    else if (strcmp(req.filename, "Files/popularity") == 0)
    {
    	/* write_table(fd, db, "SELECT Orders.Price, Users.Country FROM Orders JOIN Users ON Orders.ID = Users.ID;", 0); */
    	write_table(fd, db, "SELECT DISTINCT a.Country, '£'||CAST(IFNULL(SUM(b.Price)/100.0, 0.0) AS TEXT) `Total Spending` FROM Users a LEFT JOIN Orders b ON a.ID = b.User GROUP BY a.ID, a.Country", 0);
    	return;
    }

	/* handle file/directory requests*/
    struct stat sbuf;
    int status = 200, ffd = open(req.filename, O_RDONLY, 0);
    if(ffd <= 0){
        status = 404;
        char *msg = "File not found";
        client_error(fd, status, "Not found", msg);
    } else {
        fstat(ffd, &sbuf);
        if(S_ISREG(sbuf.st_mode)){
            if (req.end == 0){
                req.end = sbuf.st_size;
            }
            if (req.offset > 0){
                status = 206;
            }
            serve_static(fd, ffd, &req, sbuf.st_size);
        } else if(S_ISDIR(sbuf.st_mode)){
            status = 200;
            handle_directory_request(fd, ffd, req.filename);
        } else {
            status = 400;
            char *msg = "Unknow Error";
            client_error(fd, status, "Error", msg);
        }
        close(ffd);
    }
    log_access(status, clientaddr, &req);
}

int main(int argc, char** argv)
{
	/* database */
    sqlite3 *db;
    char *err_msg = 0;
    
    int rc = sqlite3_open("../Database/Database.db", &db);
    
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);   
        return 1;
    }
	
	/* web server */
    struct sockaddr_in clientaddr;
    int default_port = 9999,
        listenfd,
        connfd;
    char buf[256];
    char *path = getcwd(buf, 256);
    socklen_t clientlen = sizeof clientaddr;
    if(argc == 2) {
        if(argv[1][0] >= '0' && argv[1][0] <= '9') {
            default_port = atoi(argv[1]);
        } else {
            path = argv[1];
            if(chdir(argv[1]) != 0) {
                perror(argv[1]);
                exit(1);
            }
        }
    } else if (argc == 3) {
        default_port = atoi(argv[2]);
        path = argv[1];
        if(chdir(argv[1]) != 0) {
            perror(argv[1]);
            exit(1);
        }
    }

    listenfd = open_listenfd(default_port);
    if (listenfd > 0) {
        printf("listen on port %d, fd is %d\n", default_port, listenfd);
    } else {
        perror("ERROR");
        exit(listenfd);
    }
    
    /* ignore SIGPIPE signal, so if browser cancels the request, it won't kill the whole process. */
    signal(SIGPIPE, SIG_IGN);

    for(int i = 0; i < 10; i++)
    {
        int pid = fork();
        if (pid == 0) { /* child */
            while(1){
                connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
                process(connfd, &clientaddr, db);
                close(connfd);
            }
        } else if (pid > 0) { /* parent */
            printf("child pid is %d\n", pid);
        } else {
            perror("fork");
        }
    }

    while(1)
    {
        connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
        process(connfd, &clientaddr, db);
        close(connfd);
    }
    
    sqlite3_close(db);

    return 0;
}