# riprm

Recursive file deletion program that uses asynchronous I/O using [io_uring](https://unixism.net/loti/what_is_io_uring.html) under the hood.

Designed to very quickly free up disk space. By default, it will remove files only and keep directories (which become empty) intact to save time.

## Usage

```
riprm path/to/dir /another/dir ./yet/another/dir/ ...
```
