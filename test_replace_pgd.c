/*
 * test_uspt.c
 *
 *  Created on: Jul 13, 2017
 *      Author: spring
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

# include <unistd.h>
# include <pwd.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

typedef unsigned long int uint64_t;

struct imp_switchpgd {
	uint64_t 	addr;
	uint64_t	pgdaddr;
} __attribute__((packed));

struct imp_getpgd {
	uint64_t 	addr;
	uint64_t   pgdaddr;
	uint64_t 	size;
} __attribute__((packed));

void switch_pgd(uint64_t pgd){
	int fd;
	struct imp_switchpgd tmp;
	tmp.pgdaddr = pgd;
    fd = open("/dev/imp-virtual", O_RDWR);
    if (-1 == fd) {
        printf("%s\n", "open /dev/imp-virtual error");
    }
    int c;
    while((c = getchar()) != '\n' && c != EOF);
    int ret = ioctl(fd, 0x4010a403, &tmp);
    if(ret < 0 ){
    	printf("%s\n", "switch page table failed!");
    }
    close(fd);
}

uint64_t copy_pgd(uint64_t pgd){
	int fd;
	struct imp_getpgd tmp;

	void *necl_pgd = mmap(NULL, 10<<20, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0); //MAP_LOCKED
	memset(necl_pgd, 0, (10 << 20));
	tmp.addr = ((uint64_t)necl_pgd & (~0xfff)) + 0x1000;
	tmp.pgdaddr = (pgd & (~0xfff)) + 0x1000;
	tmp.size = (10 << 20) - (3<<12);
    fd = open("/dev/imp-virtual", O_RDWR);
    if (-1 == fd) {
        printf("%s\n", "open /dev/imp-virtual error");
    }
    int ret = ioctl(fd, 0x4018A404, &tmp);
    if(ret < 0 ){
    	printf("%s\n", "switch page table failed!");
    }
    close(fd);

    switch_pgd(tmp.addr);
    printf("i'm execute in new page table, yeah\n");
    return tmp.addr;
}

int main(){
	 copy_pgd(0);
	 return 0;
}

