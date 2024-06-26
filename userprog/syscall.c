#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

static bool fd_cmp(const struct list_elem *a, const struct list_elem *b, void*aux UNUSED){
	int a_fd, b_fd; 
	a_fd = list_entry(a, struct fd_elem, elem)->fd;
	b_fd = list_entry(b, struct fd_elem, elem)->fd;
	return a_fd < b_fd;
}

static struct file * fd_to_file(int fd){
	struct list_elem *e; 
	for (e = list_begin(thread_current()->fd_list);
		e != list_end(thread_current()->fd_list); e = list_next(e)){
			struct fd_elem * fd_elem = list_entry(e, struct fd_elem, elem);
			if (fd_elem->fd == fd)
				return fd_elem->file_ptr;
	}
	return NULL;
}

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
	g_fd_nr = 3; // why ? 
	lock_init(&syscall_lock);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	
	memcpy(&thread_current()->parent_if, f, sizeof(struct intr_frame));

	switch (f->R.rax){
		case SYS_HALT:
			SyS_halt();
			break;
		case SYS_EXIT:
			SyS_exit(f->R.rdi);
			break;
		case SYS_FORK:
			f->R.rax = SyS_fork(f->R.rdi);
			break;
		case SYS_EXEC:
			f->R.rax = SyS_exec(f->R.rdi);
			break;
		case SYS_WAIT:
			f->R.rax = SyS_wait(f->R.rdi);
			break;
		case SYS_CREATE:
			f->R.rax = SyS_create(f->R.rdi, f->R.rsi);
			break;
		case SYS_REMOVE:
			f->R.rax = SyS_remove(f->R.rdi);
			break;
		case SYS_OPEN:
			f->R.rax = SyS_open(f->R.rdi);
			break;
		case SYS_FILESIZE:
			f->R.rax = SyS_filesize(f->R.rdi);
			break;
		case SYS_READ:
			f->R.rax = SyS_read(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_WRITE:
			f->R.rax = SyS_write(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_SEEK:
			SyS_seek(f->R.rdi, f->R.rsi);
			break;
		case SYS_TELL:
			f->R.rax = SyS_tell(f->R.rdi);
			break;
		case SYS_CLOSE:
			SyS_close(f->R.rdi);
			break;

		case SYS_DUP2:
			SyS_dup2(f->R.rdi, f->R.rsi);
			break;

		default:
			printf("system call!\n");
			thread_exit();
			break;
	}

}
void check_address(void *addr){
	// Should check addr != NULL ?  
	if (!is_user_vaddr(addr))	
		SyS_exit(-1);
}

void SyS_halt(void){
	power_off();
}

void SyS_exit(int status){
	struct thread *th = thread_current();
	/*
		Verify that the program has been terminated 
		normally (0 on normal termination)
	*/ 
	th->exit_status = status; 
	printf("%s: exit(%d)\n", th->name, status);
	thread_exit();
}

pid_t SyS_fork(const char *thread_name){
	check_address(thread_name);
	pid_t child_pid = (pid_t) process_fork(thread_name, &thread_current()->parent_if);
	
	if (child_pid == TID_ERROR)
		return TID_ERROR;
	
	struct thread *child = NULL;
	struct list_elem *e;
	for (e = list_begin(&thread_current()->child_list); 
		e != list_end(&thread_current()->child_list); e = list_next(e)) {
			struct thread *tmp = list_entry(e, struct thread, child_elem);
			if (tmp->tid == child_pid){
				child = tmp;
				break;
			}
	}
	if (child == NULL)
		return TID_ERROR;
	else {
		sema_down(&child->_do_fork_sema);
		if (child->exit_status == TID_ERROR)
			return TID_ERROR;
	}
	return child_pid;
}

int SyS_exec(const char *cmd_line){
	check_address(cmd_line);
	char*copy = (char*) malloc(strlen(cmd_line) + 1);
	strlcpy(copy, cmd_line, strlen(cmd_line) + 1);
	int result = process_exec(copy);
	free(copy);
	if (result == -1)
		SyS_exit(-1);
	thread_current()->exit_status = result;
	return result;
}

int SyS_wait(pid_t pid){
	return process_wait((tid_t) pid);
}

bool SyS_create(const char *file, unsigned initial_size){
	check_address(file);
	if (file == NULL)
		SyS_exit(-1);
	lock_acquire(&syscall_lock);
	bool success = filesys_create(file, initial_size);
	lock_release(&syscall_lock);
	return success;
}

bool SyS_remove(const char *file){
	check_address(file);
	if (file == NULL)
		return false;
	lock_acquire(&syscall_lock);
	bool success = filesys_remove(file);
	lock_release(&syscall_lock);
	return success;
}

int SyS_open(const char *file) {
	check_address(file); 
	if (file == NULL)
		return -1;
	struct file *file_open = filesys_open(file);
	if (file_open == NULL)
		return -1; 
	struct fd_elem *fd_elem = malloc(sizeof(struct fd_elem));
	if (!fd_elem)
		return -1; 
	fd_elem->fd = g_fd_nr; 
	fd_elem->file_ptr = file_open; 
	g_fd_nr ++; 

	// Do not allow the file to be modified when it is opened for execution
	if (!strcmp(file, thread_current()->name))
		file_deny_write(file_open);
	
	list_insert_ordered(thread_current()->fd_list, &fd_elem->elem, fd_cmp, NULL);
	return fd_elem->fd;
}

int SyS_filesize(int fd){
	struct file *file_ptr = fd_to_file(fd);
	if (file_ptr == NULL)
		return -1;
	else
		return file_length(file_ptr);
}

int SyS_read(int fd, void *buffer, unsigned size){
	check_address(buffer);
	int count = 0;
	if (fd == 0){
		lock_acquire(&syscall_lock);
		count = input_getc();
		lock_release(&syscall_lock);
	}
	else if (fd == 1){
		SyS_exit(-1);
		return -1; 
	} 
	else {
		struct file *fd_file = fd_to_file(fd);
		if (fd_file != NULL){
			lock_acquire(&syscall_lock);
			count = file_read(fd_file, buffer, size);
			lock_release(&syscall_lock);
			return count;
		} else {
			SyS_exit(-1);
			return -1;
		}
	}
}

int SyS_write(int fd, const void * buffer, unsigned size){
	check_address(buffer);
	if (fd == 1){
		// STDOUT
		lock_acquire(&syscall_lock);
		putbuf(buffer, size);
		lock_release(&syscall_lock);
		return size;
	}
	else if (fd == 0){
		// STDIN
		return -1;
	} else {
		struct file *fd_file = fd_to_file(fd);
		if (fd_file == NULL) {
			return -1;
		} else {
			int count; 
			lock_acquire(&syscall_lock);
			count = file_write(fd_file, buffer, size);
			lock_release(&syscall_lock);
			return count;
		}
	}
}

void SyS_close(int fd){
	bool find_fd = false; 
	struct fd_elem * close_fd_elem = NULL;	 	
	struct list_elem *e;
	for (e = list_begin(thread_current()->fd_list);
		e != list_end(thread_current()->fd_list); e = list_next(e)){
			struct fd_elem *tmp = list_entry(e, struct fd_elem, elem);
			if (fd == tmp->fd){
				close_fd_elem = tmp;
				break;
			}
	}
	if (close_fd_elem == NULL)
		SyS_exit(-1);
	else {
		list_remove(e);
		lock_acquire(&syscall_lock);
		file_close(close_fd_elem->file_ptr);
		lock_release(&syscall_lock);
		free(close_fd_elem);
	}
}


void SyS_seek(int fd, unsigned position){
	file_seek(fd_to_file(fd), position);
}

unsigned SyS_tell(int fd){
	return (unsigned) file_tell(fd_to_file(fd));
}

int SyS_dup2(int old_fd, int new_fd){
	struct file* old_file = fd_to_file(old_fd);
	struct file* new_file = fd_to_file(new_fd);
	
	if (old_file == NULL)
		return -1;
	if (new_fd == old_fd)
		return new_fd;
	
	// If new_fd was previously open --> close before reused.
	if (new_file != NULL)
		SyS_close(new_fd);

	struct fd_elem *new_fd_elem = malloc(sizeof(struct fd_elem));
	new_fd_elem->file_ptr = old_fd;
	new_fd_elem->fd = new_fd;
	list_insert_ordered(thread_current()->fd_list, &new_fd_elem->elem, fd_cmp, NULL);
	
	return new_fd;
}