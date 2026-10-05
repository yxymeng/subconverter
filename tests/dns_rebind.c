/* Deterministic DNS changes for the full-binary Linux regression. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int getaddrinfo(const char *host, const char *service, const struct addrinfo *hints,
                struct addrinfo **result)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static int direct, redirect;
    int (*resolve)(const char *, const char *, const struct addrinfo *, struct addrinfo **) =
        dlsym(RTLD_NEXT, "getaddrinfo");
    const char *address = NULL;
    pthread_mutex_lock(&lock);
    if(host && strcmp(host, "direct-rebind.invalid") == 0)
        address = direct++ ? "127.0.0.1" : "127.0.0.2";
    else if(host && strcmp(host, "redirect-rebind.invalid") == 0)
        address = redirect++ ? "127.0.0.1" : "127.0.0.2";
    else if(host && strcmp(host, "safe-dns.invalid") == 0)
        address = "127.0.0.2";
    if(address)
    {
        const char *path = getenv("SUBCONVERTER_DNS_TEST_LOG");
        FILE *log = path ? fopen(path, "a") : NULL;
        if(log) { fprintf(log, "%s %s\n", host, address); fclose(log); }
    }
    pthread_mutex_unlock(&lock);
    return resolve(address ? address : host, service, hints, result);
}
