//xargs命令的作用，是将标准输入转为命令行参数。
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#define MAXARG 32  // max exec arguments

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(2, "Usage: echo hello too | xargs echo bye\n");
        exit(1);
    }
    char buf[512];
    int len = read(0, buf, sizeof(buf));

    int lines = 0;
    for (int _ = 0;_ < len;_++) {
        if (buf[_] == '\n')
            lines++;
    }
    
    char xargs[lines][64];
    int i = 0, j = 0;
    for (int _ = 0;_ < len;_++) {
        xargs[i][j++] = buf[_];
        if (buf[_] == '\n') {
            xargs[i][j - 1] = 0;
            ++i;
            j = 0;
        }
    }

    char *args[MAXARG];
    for (int i = 0;i < argc - 1;i++) // 保存原本命令
        args[i] = argv[i + 1];
    i = 0;
    while (i < lines) { // 将每一行数据拼接在原命令后
        args[argc - 1] = xargs[i++];
        if (!fork()) { // 子进程
            exec(argv[1], args);
            exit(0);
        }
        wait(0);
    }
    return 0;
}