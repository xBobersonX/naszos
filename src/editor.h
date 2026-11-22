#ifndef EDITOR_H
#define EDITOR_H

#include "fs.h"

/* Pelnoekranowy edytor tekstu. Otwiera plik `path` (wzgledem cwd), a jesli
 * go nie ma - tworzy go przy pierwszym zapisie. Wraca po Ctrl+Q. */
void editor_run(struct fs_node *cwd, const char *path);

#endif
