#define _DEFAULT_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

// TODO Tune
#define QUEUE_DEPTH 16
#define DIR_OPEN_FLAGS (O_RDONLY | O_DIRECTORY)
#define DIRENT_NAME_MAXLEN 256

typedef struct {
  int op;
  int dir_fd;
  char ent_name[DIRENT_NAME_MAXLEN];
} task_t;

task_t* task_create(int op, int dir_fd, char* ent_name) {
  // TODO Bump allocate/buffer pool
  task_t* task = malloc(sizeof(task_t));
  task->op = op;
  task->dir_fd = dir_fd;
    // We must duplicate as next readdir can overwrite d_name.
  memcpy(&task->ent_name[0], ent_name, DIRENT_NAME_MAXLEN);
  return task;
}

void task_destroy(task_t* task) {
  free(task);
}

void handle_cqe(
  struct io_uring* ring,
  int* queued,
  struct io_uring_cqe *cqe
);

void visit_dir(
  struct io_uring* ring,
  int* queued,
  int dir_fd
);

void submit(
  struct io_uring* ring,
  int* queued
) {
  io_uring_submit(ring);
  struct io_uring_cqe* cqe;
  int wait_err = io_uring_wait_cqe(ring, &cqe);
  if (wait_err < 0) {
    fprintf(stderr, "Failed to wait on queue: %s\n", strerror(-wait_err));
    exit(1);
  }
  handle_cqe(ring, queued, cqe);
  while (true) {
    int peek_err = io_uring_peek_cqe(ring, &cqe);
    if (peek_err == -EAGAIN) {
      break;
    }
    if (peek_err < 0) {
      fprintf(stderr, "Failed to peek queue: %s\n", strerror(-peek_err));
      exit(1);
    }
    handle_cqe(ring, queued, cqe);
  }
}

void increment_queued_and_maybe_submit(
  struct io_uring* ring,
  int* queued
) {
  (*queued)++;
  if (*queued == QUEUE_DEPTH) {
    submit(ring, queued);
  }
}

void handle_openat_cqe(
  struct io_uring* ring,
  int* queued,
  int dir_fd,
  char* ent_name,
  int subdir_fd
) {
  if (subdir_fd == -ENOTDIR) {
    // TODO We assume it's a file.
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    // TODO We could reuse existing task.
    task_t* task = task_create(IORING_OP_UNLINKAT, dir_fd, ent_name);
    io_uring_prep_unlinkat(sqe, dir_fd, &task->ent_name[0], 0);
    io_uring_sqe_set_data(sqe, task);
    increment_queued_and_maybe_submit(ring, queued);
  } else if (subdir_fd < 0) {
    // Ignore inaccessible items.
    // TODO Log?
  } else {
    visit_dir(ring, queued, subdir_fd);
  }
}

void handle_unlinkat_cqe(
  int result
) {
  // TODO Log errors
  (void) result;
}

void handle_cqe(
  struct io_uring* ring,
  int* queued,
  struct io_uring_cqe *cqe
) {
  task_t* task = io_uring_cqe_get_data(cqe);
  // Immediately mark as seen and decrement count as we may try to enqueue new tasks in handlers.
  io_uring_cqe_seen(ring, cqe);
  (*queued)--;
  if (task->op == IORING_OP_OPENAT) {
    handle_openat_cqe(ring, queued, task->dir_fd, &task->ent_name[0], cqe->res);
  } else {
    handle_unlinkat_cqe(cqe->res);
  }
  task_destroy(task);
}

void visit_dir(
  struct io_uring* ring,
  int* queued,
  int dir_fd
) {
  DIR* dir = fdopendir(dir_fd);
  if (dir == NULL) {
    // Ignore inaccessible folders.
    // TODO Log?
    return;
  }

  struct dirent* ent;
  while ((ent = readdir(dir)) != NULL) {
    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
      continue;
    }

    // d_type isn't set on some file systems (e.g. XFS with ftype=0); to avoid expensive stat, we try directly opening as dir.
    task_t* task = task_create(IORING_OP_OPENAT, dir_fd, ent->d_name);
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_openat(sqe, dir_fd, &task->ent_name[0], DIR_OPEN_FLAGS, 0);
    io_uring_sqe_set_data(sqe, task);
    increment_queued_and_maybe_submit(ring, queued);
  }
}

int main(int argc, char** argv) {
  struct io_uring ring;
  int ring_err = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
  if (ring_err < 0) {
    fprintf(stderr, "Failed to initialise queue: %s", strerror(-ring_err));
    exit(1);
    return 1;
  }

  int queued = 0;

  for (int i = 1; i < argc; i++) {
    char* dir_path = argv[i];
    int dir_fd = open(dir_path, DIR_OPEN_FLAGS);
    if (dir_fd < 0) {
      // Ignore inaccessible folders.
      // TODO Log?
      continue;
    }
    visit_dir(&ring, &queued, dir_fd);
  }
  while (queued) {
    submit(&ring, &queued);
  }
}
