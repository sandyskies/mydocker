#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <libcgroup.h>
#include <time.h>
#include <signal.h>

#define STACK_SIZE (1024 * 1024) 
#define MEMORY_LIMIT (512*1024*1024)

const char* rootfs = "/data1/centos6/rootfs/"; //centos6 镜像位置
const char* hostname = "mydocker"; //container 主机名
static char child_stack[STACK_SIZE];
char* const child_args[] = {
    "/bin/bash",
    NULL
};

int pipe_fd[2]; //父子进程同步

int child_main(void* args) {
    char c;
    printf("In child process(container)\n");

    chroot(rootfs); //用chroot 切换根目录
    if(errno != 0){
        perror("chroot()");
        exit(1);
    }

//clone 调用中的 CLONE_NEWUTS 起隔离主机名和域名的作用
    sethostname(hostname, sizeof(hostname));
    if( errno != 0 ){
        perror("sethostname()!");
        exit(1);
    }

//挂载proc子系统，CLONE_NEWNS 起隔离文件系统作用
    mount("proc", "/proc", "proc", 0, NULL); 
    if (errno != 0){
        perror("Mount(proc)");
        exit(1);
    }

//切换的根目录
    chdir("/");
   
    close(pipe_fd[1]);
    read(pipe_fd[0], &c, 1);
    
    //设置veth1 网络
    system("ip link set lo up");
    system("ip link set veth1 up");
    system("ip addr add 169.254.1.2/30 dev veth1");

//将子进程的镜像替换成bash
    execv(child_args[0], child_args); 
    return 1;

}

struct cgroup*  cgroup_control(pid_t pid){
    struct cgroup *cgroup = NULL;
    int ret;
    ret = cgroup_init();
    char* cgname = malloc(19*sizeof(char));
    if (ret) {
        printf("error occurs while init cgroup.\n");
        return NULL;
    }
    time_t now_time = time(NULL);
    sprintf(cgname, "mydocker_%d", (int)now_time);
    printf("%s\n", cgname);

    cgroup = cgroup_new_cgroup(cgname);
    if( !cgroup ){
        ret = ECGFAIL;
        printf("Error new cgroup%s\n", cgroup_strerror(ret));
        goto out;
    }
    
    //添加cgroup memory 和 cpuset子系统
    struct cgroup_controller *cgc = cgroup_add_controller(cgroup, "memory");
    struct cgroup_controller *cgc_cpuset = cgroup_add_controller(cgroup, "cpuset");

    if ( !cgc || !cgc_cpuset ){
        ret = ECGINVAL;
        printf("Error add controller %s\n", cgroup_strerror(ret));
        goto out;
    }

    // 内存限制  512M
    if( cgroup_add_value_uint64(cgc, "memory.limit_in_bytes", MEMORY_LIMIT) ){
        printf("Error limit memory.\n");
        goto out;
    }
    
    //限制只能使用0和1号cpu
    if (  cgroup_add_value_string(cgc_cpuset, "cpuset.cpus", "0-1") ){
        printf("Error limit cpuset cpus.\n");
        goto out;
    } 
    //限制只能使用0和1块内存
    if (  cgroup_add_value_string(cgc_cpuset, "cpuset.mems", "0-1") ){
        printf("Error limit cpuset mems.\n");
        goto out;
    } 
    ret = cgroup_create_cgroup(cgroup, 0);
    if (ret){
        printf("Error create cgroup%s\n", cgroup_strerror(ret));
        goto out;
    }
    ret = cgroup_attach_task_pid(cgroup, pid);
    if (ret){
        printf("Error attach_task_pid %s\n", cgroup_strerror(ret));
        goto out;
    }
    
    return cgroup; 
out:
    if (cgroup){
        cgroup_delete_cgroup(cgroup, 0); 
        cgroup_free(&cgroup);
    }
    return NULL;

}

int main() {
    char* cmd;
    printf("main process: \n");

    pipe(pipe_fd);
    if( errno != 0){
        perror("pipe()");
        exit(1);
    }

    int child_pid = clone(child_main, child_stack + STACK_SIZE, \
            CLONE_NEWNET | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS | SIGCHLD, NULL);

    struct cgroup* cg = cgroup_control(child_pid);

    //添加veth pair ，设置veth1 namespace 为子进程的，veth0 在父进程的namespace
    system("ip link add veth0 type veth peer name veth1");
    asprintf(&cmd, "ip link set veth1 netns %d", child_pid);
    system(cmd);
    system("ip link set veth0 up");
    system("ip addr add 169.254.1.1/30 dev veth0");
    free(cmd);

    //等执行以上命令，通知子进程，子进程设置自己的网络
    close(pipe_fd[1]);  

    waitpid(child_pid, NULL, 0);
    if (cg) {
        cgroup_delete_cgroup(cg, 0); //删除cgroup 子系统
    }
    printf("child process exited.\n");
    return 0;
}

