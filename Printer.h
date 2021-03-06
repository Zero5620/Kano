#pragma once
#include "CodeNode.h"
#include <stdio.h>

void print_syntax(Syntax_Node *root, FILE *fp = stdout, int child_indent = 0, const char *title = nullptr);
void print_code(Code_Node *root, FILE *fp = stdout, int child_indent = 0, const char *title = nullptr);
