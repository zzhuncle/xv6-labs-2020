#include "kernel/types.h"
#include "user/user.h"

void primes(int *pl)
{
    close(pl[1]);// 关闭左侧写端

    int prime;
    // Hint: read returns zero when the write-side of a pipe is closed.
    if (read(pl[0], &prime, sizeof(prime)) == 0) {
        exit(0);
    }
    printf("prime %d\n", prime);

    int pr[2];
    pipe(pr);
    int pid = fork();
    if (!pid) {// 子进程
        primes(pr);
    } else {// 父进程
        close(pr[0]); // 关闭右侧读端
        for (int num; read(pl[0], &num, sizeof(num)) != 0;) {
            if (num % prime) {
                write(pr[1], &num, sizeof(num));
            }
        }
        close(pl[0]); // 关闭左侧读端
        close(pr[1]); // 关闭右侧写端
        wait(0);
    }
}

int main(int argc, char *argv[])
{
    int p[2];
    pipe(p);
    int pid = fork();
    if (pid) {// 父进程
        close(p[0]);// 关闭读端
        for (int i = 2;i <= 35;i++) {
            write(p[1], &i, sizeof(i));
        }
        close(p[1]);// 关闭写端
        wait(0);
    } else {// 子进程
        primes(p);
    }
    exit(0);
}
