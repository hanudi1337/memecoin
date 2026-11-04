// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "args.h"

#include "app.h"
#include "log.h"

#include <string.h>

void pv_args_init(PvArgs *a, const char *cmd, int argc, char **argv)
{
	a->cmd = cmd;
	a->argc = argc;
	a->argv = argv;
	a->idx = -1;
	a->cur = NULL;
	a->failed = false;
}

bool pv_args_next(PvArgs *a)
{
	if(a->failed)
		return false;
	a->idx++;
	if(a->idx >= a->argc)
	{
		a->cur = NULL;
		return false;
	}
	a->cur = a->argv[a->idx];
	return true;
}

bool pv_args_flag(PvArgs *a, const char *name)
{
	if(!a->cur || a->failed)
		return false;
	if(strcmp(a->cur, name) == 0)
		return true;
	size_t n = strlen(name);
	if(strncmp(a->cur, name, n) == 0 && a->cur[n] == '=')
	{
		LOGE("args", "Option %s erwartet keinen Wert (\"%s\").", name, a->cur);
		a->failed = true;
	}
	return false;
}

bool pv_args_value(PvArgs *a, const char *name, const char **value)
{
	if(!a->cur || a->failed)
		return false;
	size_t n = strlen(name);
	if(strncmp(a->cur, name, n) != 0)
		return false;
	if(a->cur[n] == '=')
	{
		*value = a->cur + n + 1;
		return true;
	}
	if(a->cur[n] != '\0')
		return false;
	if(a->idx + 1 >= a->argc)
	{
		LOGE("args", "Option %s braucht einen Wert, z. B. %s <wert>.", name, name);
		a->failed = true;
		return false;
	}
	const char *next = a->argv[a->idx + 1];
	// "--host --pin 123" is almost certainly a missing value, not a host named "--pin"
	if(next[0] == '-' && next[1] == '-' && next[2] != '\0')
	{
		LOGE("args", "Option %s braucht einen Wert, gefunden wurde aber die Option \"%s\".", name, next);
		a->failed = true;
		return false;
	}
	a->idx++;
	*value = next;
	return true;
}

bool pv_args_is_help(PvArgs *a)
{
	return a->cur && (strcmp(a->cur, "-h") == 0 || strcmp(a->cur, "--help") == 0 || strcmp(a->cur, "/?") == 0);
}

int pv_args_unknown(PvArgs *a)
{
	if(a->failed)
		return PV_EXIT_USAGE;
	if(a->cur && a->cur[0] == '-')
		LOGE("args", "Unbekannte Option \"%s\" für \"%s\". Hilfe: ps5view %s --help", a->cur, a->cmd, a->cmd);
	else
		LOGE("args", "Unerwartetes Argument \"%s\" für \"%s\". Hilfe: ps5view %s --help", a->cur ? a->cur : "", a->cmd, a->cmd);
	a->failed = true;
	return PV_EXIT_USAGE;
}
