#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <memory.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <assert.h>
#include <pthread.h>
#include <time.h>
#include <sys/times.h>
#include <sys/sendfile.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <resolv.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>

#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <openssl/modes.h>
#include <openssl/aes.h>


int bytes_recv;
int port;
char* test_data;

#define KTLS_SET_IV_RECV                1
#define KTLS_SET_KEY_RECV               2
#define KTLS_SET_SALT_RECV              3
#define KTLS_SET_IV_SEND                4
#define KTLS_SET_KEY_SEND               5
#define KTLS_SET_SALT_SEND              6
#define KTLS_SET_MTU                    7
#define KTLS_UNATTACH                   8
#define KTLS_VERSION_1_2                1
#define AF_KTLS         12
#define KTLS_CIPHER_AES_GCM_128         51
struct sockaddr_ktls {
        unsigned short   sa_cipher;
        unsigned short   sa_socket;
        unsigned short   sa_version;
};

/* Opaque OpenSSL structures to fetch keys */
#define u64 uint64_t
#define u32 uint32_t
#define u8 uint8_t

typedef struct {
  u64 hi, lo;
} u128;

typedef struct {
  /* Following 6 names follow names in GCM specification */
  union {
    u64 u[2];
    u32 d[4];
    u8 c[16];
    size_t t[16 / sizeof(size_t)];
  } Yi, EKi, EK0, len, Xi, H;
  /*
   * Relative position of Xi, H and pre-computed Htable is used in some
   * assembler modules, i.e. don't change the order!
   */
#if TABLE_BITS==8
  u128 Htable[256];
#else
  u128 Htable[16];
  void (*gmult) (u64 Xi[2], const u128 Htable[16]);
  void (*ghash) (u64 Xi[2], const u128 Htable[16], const u8 *inp,
                 size_t len);
#endif
  unsigned int mres, ares;
  block128_f block;
  void *key;
} gcm128_context_alias;

typedef struct {
  union {
    double align;
    AES_KEY ks;
  } ks;                       /* AES key schedule to use */
  int key_set;                /* Set if key initialised */
  int iv_set;                 /* Set if an iv is set */
  gcm128_context_alias gcm;
  unsigned char *iv;          /* Temporary IV store */
  int ivlen;                  /* IV length */
  int taglen;
  int iv_gen;                 /* It is OK to generate IVs */
  int tls_aad_len;            /* TLS AAD length */
  ctr128_f ctr;
} EVP_AES_GCM_CTX;


/* AF_ALG defines not in linux headers */
#ifndef AF_ALG
#define AF_ALG 38
#endif
#ifndef SOL_ALG
#define SOL_ALG 279
#endif

#ifndef ALG_SET_AEAD_ASSOCLEN
#define ALG_SET_AEAD_ASSOCLEN 4
#endif
#ifndef ALG_SET_AEAD_AUTHSIZE
#define ALG_SET_AEAD_AUTHSIZE 5
#endif
#ifndef ALG_SET_PUBKEY
#define ALG_SET_PUBKEY 6
#endif

pthread_t server_thread;

void *main_server(void*);
int main_tls_client(void);

int main(int argv, char* argc[]) {

  if (argv != 3) {
    printf("usage: ./tls port test_data_file\n");
    exit(-1);
  }
  port = atoi(argc[1]);
  printf("Serving port %i\n", port);
  test_data = argc[2];

  SSL_library_init();
  OpenSSL_add_all_algorithms();
  ERR_load_BIO_strings();
  ERR_load_crypto_strings();
  SSL_load_error_strings();/* load all error messages */

  int rc = pthread_create(&server_thread, NULL, main_server, NULL);
  if (rc) {
    printf("Error creating server %i\n", rc);
    exit(-1);
  }
  sleep(2);

  main_tls_client();
  return 0;
}

int create_socket() {
  int sockfd;
  struct sockaddr_in6 dest_addr;

  sockfd = socket(AF_INET6, SOCK_STREAM, 0);

  memset(&(dest_addr), '\0', sizeof(dest_addr));
  dest_addr.sin6_family=AF_INET6;
  dest_addr.sin6_port=htons(port);

  inet_pton(AF_INET6, "::1", &dest_addr.sin6_addr.s6_addr);

  if ( connect(sockfd, (struct sockaddr *) &dest_addr,
               sizeof(struct sockaddr_in6)) == -1 ) {
    perror("Connect: ");
    exit(-1);
  }

  return sockfd;
}

int main_tls_client() {
  SSL_CTX *ctx;
  SSL *ssl;
  int server = 0;
  int ret;

  if ( (ctx = SSL_CTX_new(SSLv23_client_method())) == NULL)
    printf("Unable to create a new SSL context structure.\n");

  SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2);
  // Force gcm(aes) mode
  SSL_CTX_set_cipher_list(ctx, "ECDH-ECDSA-AES128-GCM-SHA256");

  ssl = SSL_new(ctx);

  server = create_socket();

  SSL_set_fd(ssl, server);

  if ( SSL_connect(ssl) != 1 ) {
    printf("Error: Could not build a SSL session\n");
    exit(-1);
  }

// Start tests

  clock_t start, end;
  double cpu_time_used;

  int filefd;
  int bytes;
  int totalbytes = 0;
  bytes_recv = 0;
  char buf[16384];

  int res = 0;
  int total_recv = 0;

  start = clock();

  filefd = open(test_data, O_RDONLY);
  totalbytes = 0;

  do {
    bytes = read(filefd, buf, sizeof(buf));
    totalbytes += bytes;
    if (bytes > 0)
      SSL_write(ssl, buf, bytes);
  } while(bytes > 0);

  close(filefd);


    end = clock();
    cpu_time_used = ((double) (end - start)) / CLOCKS_PER_SEC;

    printf("OpenSSL receive time: %.02f\n", cpu_time_used);

  res = 0;
  total_recv = 0;

  res = SSL_read(ssl, buf, 1);

  total_recv += res;
  if (res < 0) {
    printf("SSL Read error: %i\n", res);
  }
  printf("Received openssl test data: %i %i\n", res, total_recv);

/* Kernel TLS tests */
  int tfmfd = socket(AF_KTLS, SOCK_STREAM, 0);
  if (tfmfd == -1) {
    perror("socket error:");
    exit(-1);
  }

  struct sockaddr_ktls sa = {
                .sa_cipher = KTLS_CIPHER_AES_GCM_128,
                .sa_socket = server,
                .sa_version = KTLS_VERSION_1_2,
  };

  if (bind(tfmfd, (struct sockaddr *)&sa, sizeof(sa)) == -1) {
    perror("AF_ALG: bind failed");
    close(tfmfd);
    exit(-1);
  }

  EVP_CIPHER_CTX * writeCtx = ssl->enc_write_ctx;
  EVP_CIPHER_CTX * readCtx = ssl->enc_read_ctx;

  EVP_AES_GCM_CTX* gcmWrite = (EVP_AES_GCM_CTX*)(writeCtx->cipher_data);
  EVP_AES_GCM_CTX* gcmRead = (EVP_AES_GCM_CTX*)(readCtx->cipher_data);

  unsigned char* writeKey = (unsigned char*)(gcmWrite->gcm.key);
  unsigned char* readKey = (unsigned char*)(gcmRead->gcm.key);

  unsigned char* writeIV = gcmWrite->iv;
  unsigned char* readIV = gcmRead->iv;

  char keyiv[20] = {0};
  memcpy(keyiv, writeKey, 16);
  if (setsockopt(tfmfd, AF_KTLS, KTLS_SET_KEY_SEND, keyiv, 16)) {
    perror("AF_ALG: set write key failed\n");
    exit(-1);
  }
  memcpy(keyiv, writeIV, 4);
  if (setsockopt(tfmfd, AF_KTLS, KTLS_SET_SALT_SEND, keyiv, 4)) {
    perror("AF_ALG: set write iv failed\n");
    exit(-1);
  }

  uint64_t writeSeq;
  unsigned char* writeSeqNum = ssl->s3->write_sequence;
  memcpy(&writeSeq, writeSeqNum, sizeof(writeSeq));
  if (setsockopt(tfmfd, AF_KTLS, KTLS_SET_IV_SEND, (unsigned char*)&writeSeq, 8)) {
    perror("AF_ALG: set write salt failed\n");
    exit(-1);
  }


  memcpy(keyiv, readKey, 16);
  if (setsockopt(tfmfd, AF_KTLS, KTLS_SET_KEY_RECV, keyiv, 16)) {
    perror("AF_ALG: set read key failed\n");
    exit(-1);
  }
  memcpy(keyiv, readIV, 4);
  if (setsockopt(tfmfd, AF_KTLS, KTLS_SET_SALT_RECV, keyiv, 4)) {
    perror("AF_ALG: set read iv failed\n");
    exit(-1);
  }

  uint64_t readSeq;
  unsigned char* readSeqNum = ssl->s3->read_sequence;
  memcpy(&readSeq, readSeqNum, sizeof(readSeq));
  if (setsockopt(tfmfd, AF_KTLS, KTLS_SET_IV_RECV, (unsigned char*)&readSeq, 8)) {
    perror("AF_ALG: set read salt failed\n");
    exit(-1);
  }

  send(tfmfd, "abcdefghij", 10, 0);
  printf("Successful send\n");

  res = 0;
  total_recv = 0;

  res = recv(tfmfd, &buf, 1, 0);
  printf("recv: %d  %s\n", res, buf);

  total_recv += res;
  if (res < 0) {
    printf("Ktls recv error: %i\n", res);
  }
  printf("Recvd ktls test data: %i %i\n", res, total_recv);

  start = clock();
  off_t offset = 0;


  filefd = open(test_data, O_RDONLY);

  res = sendfile(tfmfd, filefd, &offset, totalbytes);

  close(filefd);

  end = clock();
  cpu_time_used = ((double) (end - start)) / CLOCKS_PER_SEC;

  printf("ktls receive time: %.02f\n", cpu_time_used);

  SSL_free(ssl);
  close(server);
  SSL_CTX_free(ctx);
  return(0);
}

int OpenListener(int port)
{   int sd;
  struct sockaddr_in6 addr;

  sd = socket(PF_INET6, SOCK_STREAM, 0);
  bzero(&addr, sizeof(addr));
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(port);
  memcpy(addr.sin6_addr.s6_addr, &in6addr_any, sizeof(in6addr_any));

  if ( bind(sd, (const struct sockaddr*)&addr, sizeof(addr)) != 0 )
    {
      perror("can't bind port");
      abort();
    }
  if ( listen(sd, 10) != 0 )
    {
      perror("Can't configure listening port");
      abort();
    }
  return sd;
}

SSL_CTX* InitServerCTX(void)
{
  SSL_CTX *ctx;

  ctx = SSL_CTX_new(SSLv23_server_method());/* create new context from method */

  if ( ctx == NULL )
    {
      ERR_print_errors_fp(stderr);
      abort();
    }
  return ctx;
}

void LoadCertificates(SSL_CTX* ctx, char* CertFile, char* KeyFile)
{
  /* set the local certificate from CertFile */
  if ( SSL_CTX_use_certificate_file(ctx, CertFile, SSL_FILETYPE_PEM) <= 0 )
    {
      ERR_print_errors_fp(stderr);
      abort();
    }
  /* set the private key from KeyFile (may be the same as CertFile) */
  if ( SSL_CTX_use_PrivateKey_file(ctx, KeyFile, SSL_FILETYPE_PEM) <= 0 )
    {
      ERR_print_errors_fp(stderr);
      abort();
    }
  /* verify private key */
  if ( !SSL_CTX_check_private_key(ctx) )
    {
      fprintf(stderr, "Private key does not match the public certificate\n");
      abort();
    }
}

void Servlet(int client, SSL* ssl)/* Serve the connection -- threadable */
{
  char buf[16384];
  int sd, bytes;

  if ( SSL_accept(ssl) == -1 ) {
    ERR_print_errors_fp(stderr);
  } else {
    // Send a little data to test reads
    bytes = SSL_write(ssl, buf, 1);
    bytes = SSL_write(ssl, buf, 1);
    bytes = SSL_write(ssl, buf, 1);
    bytes = SSL_write(ssl, buf, 1);

    do {

      bytes = SSL_read(ssl, buf, sizeof(buf));/* get request */
      if ( bytes > 0 )
        {
          printf("Bytes content: %i   -->%s\n", bytes_recv, buf);
        } else if (bytes == 0) {
        printf("Bytes recv: %i\n", bytes_recv);
      }
      else {
        printf("ERROR\n");
        ERR_print_errors_fp(stderr);
        break;
      }
      bytes_recv += bytes;
    } while (bytes > 0 );
  }
  sd = SSL_get_fd(ssl);/* get socket connection */
  SSL_free(ssl);/* release SSL state */
  close(sd);/* close connection */
}

void *main_server(void* unused)
{
  SSL_CTX *ctx;

  ctx = InitServerCTX();/* initialize SSL */
  LoadCertificates(ctx, "ca.crt", "ca.pem");/* load certs */
  SSL_CTX_set_cipher_list(ctx, "ECDH-ECDSA-AES128-GCM-SHA256");

  int server = OpenListener(port);/* create server socket */
  while (1)
    {
      struct sockaddr_in addr;
      unsigned int len = sizeof(addr);
      SSL *ssl;

      int client = accept(server, (struct sockaddr*) &addr, &len);/* accept connection as usual */

      ssl = SSL_new(ctx);         /* get new SSL state with context */
      SSL_set_fd(ssl, client);/* set connection socket to SSL state */
      Servlet(client, ssl);/* service connection */
    }
  close(server);/* close server socket */
  SSL_CTX_free(ctx);/* release context */

  return NULL;
}
