#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "menu.h"
#include "wayland.h"
#include "xdg-activation-v1-client-protocol.h"

static void read_items(struct menu *menu) {
	char *path = strdup(getenv("PATH"));
	for (char *p = strtok(path, ":"); p != NULL; p = strtok(NULL, ":")) {
		DIR *dir = opendir(p);
		if (dir == NULL) {
			continue;
		}
		for (struct dirent *ent = readdir(dir); ent != NULL; ent = readdir(dir)) {
			if (ent->d_name[0] == '.') {
				continue;
			}
			menu_add_item(menu, strdup(ent->d_name));
		}
		closedir(dir);
	}
	menu_sort_and_deduplicate(menu);
	free(path);
}

struct command {
	struct menu *menu;
	char *text;
	char *terminal;
	bool exit;
};

static void activation_token_done(void *data, struct xdg_activation_token_v1 *activation_token,
	const char *token) {
	struct command *cmd = data;
	xdg_activation_token_v1_destroy(activation_token);

	int pid = fork();
	if (pid == 0) {
		setenv("XDG_ACTIVATION_TOKEN", token, true);
		char *command = cmd->text;
		if (cmd->terminal != NULL) {
			size_t len = strlen(cmd->terminal) + 1 + strlen(cmd->text) + 1;
			command = calloc(len, sizeof(char));
			snprintf(command, len, "%s %s", cmd->terminal, cmd->text);
		}
		char *argv[] = {"/bin/sh", "-c", command, NULL};
		execvp(argv[0], (char**)argv);
	} else {
		if (cmd->exit) {
			cmd->menu->exit = true;
		}
	}
}

static const struct xdg_activation_token_v1_listener activation_token_listener = {
	.done = activation_token_done,
};

static char *terminal = NULL;
static char *terminal_patterns = NULL;

static bool should_use_terminal(const char *text) {
	if (terminal == NULL) {
		return false;
	}
	if (terminal_patterns == NULL) {
		return true;
	}

	char *patterns = strdup(terminal_patterns);
	for (char *pattern = strtok(patterns, ","); pattern != NULL; pattern = strtok(NULL, ",")) {
		if (fnmatch(pattern, text, 0) == 0) {
			free(patterns);
			return true;
		}
	}
	free(patterns);
	return false;
}

static void exec_item(struct menu *menu, char *text, bool exit) {
	struct command *cmd = calloc(1, sizeof(struct command));
	cmd->menu = menu;
	cmd->text = strdup(text);
	cmd->terminal = should_use_terminal(text) ? terminal : NULL;
	cmd->exit = exit;

	struct xdg_activation_v1 *activation = context_get_xdg_activation(menu->context);
	struct xdg_activation_token_v1 *activation_token = xdg_activation_v1_get_activation_token(activation);
	xdg_activation_token_v1_set_surface(activation_token, context_get_surface(menu->context));
	xdg_activation_token_v1_add_listener(activation_token, &activation_token_listener, cmd);
	xdg_activation_token_v1_commit(activation_token);
}

static void getopts(int *argc, char ***argv) {
	const char *usage =
		"Usage: wmenu-run [-T terminal] [-t patterns] [wmenu options]\n"
		"\t-T, --terminal terminal  prefix matching commands with a terminal program\n"
		"\t-t, --terminal-patterns patterns  comma-separated shell patterns for commands that should use -T\n";

	char **filtered = calloc(*argc + 1, sizeof(char *));
	int filtered_argc = 1;
	filtered[0] = (*argv)[0];

	for (int i = 1; i < *argc; ++i) {
		char *arg = (*argv)[i];
		if (strcmp(arg, "-T") == 0 || strcmp(arg, "--terminal") == 0) {
			if (i + 1 >= *argc) {
				fprintf(stderr, "%s", usage);
				exit(EXIT_FAILURE);
			}
			terminal = (*argv)[++i];
			continue;
		}
		if (strcmp(arg, "-t") == 0 || strcmp(arg, "--terminal-patterns") == 0) {
			if (i + 1 >= *argc) {
				fprintf(stderr, "%s", usage);
				exit(EXIT_FAILURE);
			}
			terminal_patterns = (*argv)[++i];
			continue;
		}
		if (strncmp(arg, "--terminal=", 11) == 0) {
			terminal = arg + 11;
			continue;
		}
		if (strncmp(arg, "--terminal-patterns=", 20) == 0) {
			terminal_patterns = arg + 20;
			continue;
		}
		if (strcmp(arg, "--help") == 0) {
			fprintf(stderr, "%s", usage);
			exit(EXIT_FAILURE);
			continue;
		}
		filtered[filtered_argc++] = arg;
	}

	filtered[filtered_argc] = NULL;
	*argc = filtered_argc;
	*argv = filtered;
	optind = 1;
}

int main(int argc, char *argv[]) {
	struct menu *menu = menu_create(exec_item);
	getopts(&argc, &argv);
	menu_getopts(menu, argc, argv);
	read_items(menu);
	int status = menu_run(menu);
	menu_destroy(menu);
	return status;
}
