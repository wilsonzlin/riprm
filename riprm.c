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

void handle_cqe(
  struct io_uring* ring,
  int* queued,
  struct io_uring_cqe *cqe
) {
  char* file_name = io_uring_cqe_get_data(cqe);
  // TODO Log errors
  free(file_name);
  io_uring_cqe_seen(ring, cqe);
  (*queued)--;
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
    int subdir_fd = openat(dir_fd, ent->d_name, DIR_OPEN_FLAGS);
    // WARNING: Check return value first as only on error is `errno` set.
    if (subdir_fd < 0 && errno == ENOTDIR) {
      // TODO We assume it's a file.
      struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
      // We must duplicate as next readdir can overwrite this data.
      // TODO Bump allocate/buffer pool
      char* file_name = strdup(ent->d_name);
      io_uring_prep_unlinkat(sqe, dir_fd, file_name, 0);
      io_uring_sqe_set_data(sqe, file_name);
      (*queued)++;
      if (*queued == QUEUE_DEPTH) {
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
    } else if (subdir_fd < 0) {
      // Ignore inaccessible items.
      // TODO Log?
    } else {
      visit_dir(ring, queued, subdir_fd);
    }
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
  io_uring_submit(&ring);

  // Since we currently don't do anything on completion (not even logging on errors), don't bother cleaning up everything (closing file descriptors, processing all completions, freeing all memory).
  struct io_uring_cqe* cqe;
  io_uring_wait_cqe_nr(&ring, &cqe, queued);
}
