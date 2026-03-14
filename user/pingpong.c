#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    // 一根管道用于父进程发给子进程，另一根用于子进程回给父进程
    int parent_to_child[2];
    int child_to_parent[2];
    char byte = 'x';
    int pid;

    // 检查pipe调用是否成功
    if (pipe(parent_to_child) < 0 || pipe(child_to_parent) < 0) {
        fprintf(2, "pingpong: pipe failed\n");
        exit(1);
    }

    // fork一个子进程
    pid = fork();
    if (pid < 0) {
        fprintf(2, "pingpong: fork failed\n");
        exit(1);
    }

    if (pid == 0) {
        // 子进程只需要从第一根管道读、向第二根管道写
        // close掉不需要的端口，避免死锁
        close(parent_to_child[1]);
        close(child_to_parent[0]);

        if (read(parent_to_child[0], &byte, 1) != 1) {
            fprintf(2, "pingpong: child read failed\n");
            exit(1);
        }
        printf("%d: received ping\n", getpid());
        if (write(child_to_parent[1], &byte, 1) != 1) {
            fprintf(2, "pingpong: child write failed\n");
            exit(1);
        }

        // 通信结束后关闭仍持有的端口
        close(parent_to_child[0]);
        close(child_to_parent[1]);
        exit(0);
    }
    
    // 父进程只需要向第一根管道写、从第二根管道读
    close(parent_to_child[0]);
    close(child_to_parent[1]);

    if (write(parent_to_child[1], &byte, 1) != 1) {
        fprintf(2, "pingpong: parent write failed\n");
        exit(1);
    }
    if (read(child_to_parent[0], &byte, 1) != 1) {
        fprintf(2, "pingpong: parent read failed\n");
        exit(1);
    }
    printf("%d: received pong\n", getpid());

    close(parent_to_child[1]);
    close(child_to_parent[0]);
    wait(0);
    exit(0);
}
