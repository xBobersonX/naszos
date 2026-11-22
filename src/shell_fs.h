#ifndef SHELL_FS_H
#define SHELL_FS_H

#include <stddef.h>

/* Komendy powloki do obslugi plikow (src/shell_fs.c) */
void cmd_ls(char *args);
void cmd_cd(char *args);
void cmd_pwd(char *args);
void cmd_mkdir(char *args);
void cmd_touch(char *args);
void cmd_write(char *args);
void cmd_append(char *args);
void cmd_cat(char *args);
void cmd_edit(char *args);
void cmd_rm(char *args);

/* Sciezka biezacego katalogu - do znaku zachety */
void shell_cwd_path(char *buf, size_t size);

#endif
