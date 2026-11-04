// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// ps5view - view-only PS5 Remote Play window for screen sharing.
// Copyright (C) 2026 ps5view contributors.
// Based on libchiaki from chiaki-ng (https://github.com/streetpea/chiaki-ng).
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU Affero General Public License version 3 as
// published by the Free Software Foundation (see COPYING), with the
// additional OpenSSL linking permission stated there.

#include "app.h"
#include "log.h"
#include "pv_version.h"
#include "util.h"

#include <chiaki/common.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>



typedef struct
{
	const char *name;
	int (*fn)(int argc, char **argv);
	const char *summary;
} Command;

static const Command commands[] = {
	{ "discover", pv_cmd_discover, "PS5 im Netz suchen (IP, Name, Status)" },
	{ "account-id", pv_cmd_account_id, "PSN-Account-ID per Sony-Login ermitteln" },
	{ "register", pv_cmd_register, "Konsole registrieren (PIN aus \"Gerät verknüpfen\")" },
	{ "profiles", pv_cmd_profiles, "Gespeicherte Registrierungen anzeigen" },
	{ "remove-profile", pv_cmd_remove_profile, "Registrierung löschen" },
	{ "stream", pv_cmd_stream, "Verbinden und PS5-Bild/-Ton im Fenster zeigen" },
	{ "list-audio", pv_cmd_list_audio, "Audio-Ausgabegeräte auflisten" },
	{ "check", pv_cmd_check, "Diagnose: Netzwerk, Konsole, Audio, Decoder" },
};

static void print_usage(void)
{
	pv_out("ps5view %s - PS5-Bild und -Ton in einem Fenster (nur Zuschauen, sendet keine Eingaben)", PV_VERSION);
	pv_out("%s", "");
	pv_out("Aufruf: ps5view <befehl> [optionen]");
	pv_out("%s", "");
	pv_out("Befehle:");
	for(size_t i = 0; i < PV_ARRAY_SIZE(commands); i++)
		pv_out("  %-15s %s", commands[i].name, commands[i].summary);
	pv_out("  %-15s %s", "version", "Version anzeigen");
	pv_out("  %-15s %s", "license", "Lizenz und Bezugsquelle des Quellcodes");
	pv_out("%s", "");
	pv_out("Globale Optionen (an beliebiger Stelle):");
	pv_out("  -v, --verbose        ausführliche Ausgabe, inkl. libchiaki-Meldungen");
	pv_out("  --trace              zusätzlich libchiaki-Hexdumps (enthalten Schlüssel, nicht weitergeben!)");
	pv_out("  --config <datei>     andere Konfigurationsdatei verwenden");
	pv_out("  --log-file <datei>   Logdatei festlegen");
	pv_out("  --no-log-file        keine Logdatei schreiben");
	pv_out("%s", "");
	pv_out("Typischer Ablauf:");
	pv_out("  ps5view discover");
	pv_out("  ps5view account-id");
	pv_out("  ps5view register --host 192.168.178.88 --account-id <id> --pin <8 Ziffern>");
	pv_out("  ps5view list-audio");
	pv_out("  ps5view stream --audio-device \"CABLE Input\" --wake");
	pv_out("%s", "");
	pv_out("Hilfe zu einem Befehl: ps5view <befehl> --help");
}

static void print_version(void)
{
	pv_out("ps5view %s (git %s)", PV_VERSION, PV_GIT_COMMIT);
	pv_out("libchiaki aus chiaki-ng, Commit %s", PV_CHIAKI_COMMIT);
	pv_out("Lizenz: GNU AGPL-3.0 mit OpenSSL-Ausnahme - Quellcode: %s", PV_SOURCE_URL);
}

static void print_license(void)
{
	pv_out("ps5view %s", PV_VERSION);
	pv_out("%s", "");
	pv_out("Dieses Programm ist freie Software unter der GNU Affero General Public License,");
	pv_out("Version 3 (nur diese Version), mit der zusätzlichen Erlaubnis zum Linken mit OpenSSL");
	pv_out("(siehe Datei COPYING). Es wird OHNE JEDE GEWÄHRLEISTUNG bereitgestellt.");
	pv_out("%s", "");
	pv_out("Es verwendet libchiaki aus chiaki-ng (AGPL-3.0): %s", PV_CHIAKI_URL);
	pv_out("  verwendeter Commit: %s", PV_CHIAKI_COMMIT);
	pv_out("%s", "");
	pv_out("Vollständiger Quellcode dieser Version (inkl. Build-Anleitung):");
	pv_out("  %s (git %s)", PV_SOURCE_URL, PV_GIT_COMMIT);
	pv_out("Wer eine veränderte Version weitergibt, muss deren Quellcode ebenfalls anbieten.");
}

// The log must not contain the PIN; the account id is only partially shown.
static void log_command_line(int argc, char **argv)
{
	char line[2048] = "";
	bool mask_next = false;
	for(int i = 0; i < argc; i++)
	{
		const char *a = argv[i];
		char buf[300];
		if(mask_next)
		{
			snprintf(buf, sizeof(buf), "***");
			mask_next = false;
		}
		else if(strcmp(a, "--pin") == 0 || strcmp(a, "--login-pin") == 0 || strcmp(a, "--account-id") == 0)
		{
			snprintf(buf, sizeof(buf), "%s", a);
			mask_next = true;
		}
		else if(pv_str_starts_with(a, "--pin=") || pv_str_starts_with(a, "--login-pin=") || pv_str_starts_with(a, "--account-id="))
			snprintf(buf, sizeof(buf), "%.*s=***", (int)(strchr(a, '=') - a), a);
		else if(strchr(a, ' '))
			snprintf(buf, sizeof(buf), "\"%s\"", a);
		else
			snprintf(buf, sizeof(buf), "%s", a);
		if(i)
			strncat(line, " ", sizeof(line) - strlen(line) - 1);
		strncat(line, buf, sizeof(line) - strlen(line) - 1);
	}
	LOGD("main", "Aufruf: %s", line);
}

static char *default_log_path(void)
{
	char *state = pv_default_state_dir();
	if(!state)
		return NULL;
	char *dir = pv_path_join(state, "logs");
	free(state);
	if(!dir)
		return NULL;
	char ts[32];
	pv_format_time(ts, sizeof(ts), PV_TIME_FILENAME);
	char name[96];
	snprintf(name, sizeof(name), "ps5view-%s-%lu.log", ts, pv_process_id());
	char *p = pv_path_join(dir, name);
	free(dir);
	return p;
}

int main(int argc_raw, char **argv_raw)
{
	pv_console_init();
	int argc;
	char **argv = pv_utf8_argv(argc_raw, argv_raw, &argc);

	// Global options may appear anywhere; everything else is passed on.
	char **rest = calloc((size_t)argc + 1, sizeof(char *));
	if(!rest)
		return PV_EXIT_INTERNAL;
	int nrest = 0;
	const char *log_file = NULL;
	bool no_log_file = false;
	bool want_version = false, want_help = false, want_license = false;
	for(int i = 1; i < argc; i++)
	{
		const char *a = argv[i];
		if(strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0)
			pv_global.verbose = true;
		else if(strcmp(a, "--trace") == 0)
			pv_global.verbose = pv_global.trace = true;
		else if(strcmp(a, "--config") == 0 && i + 1 < argc)
			pv_global.config_path = argv[++i];
		else if(pv_str_starts_with(a, "--config="))
			pv_global.config_path = a + strlen("--config=");
		else if(strcmp(a, "--log-file") == 0 && i + 1 < argc)
			log_file = argv[++i];
		else if(pv_str_starts_with(a, "--log-file="))
			log_file = a + strlen("--log-file=");
		else if(strcmp(a, "--no-log-file") == 0)
			no_log_file = true;
		else if(nrest == 0 && (strcmp(a, "--version") == 0 || strcmp(a, "version") == 0))
			want_version = true;
		else if(nrest == 0 && (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0 || strcmp(a, "help") == 0 || strcmp(a, "/?") == 0))
			want_help = true;
		else if(nrest == 0 && strcmp(a, "license") == 0)
			want_license = true;
		else
			rest[nrest++] = argv[i];
	}

	pv_log_set_console_level(pv_global.verbose ? PV_LOG_DEBUG : PV_LOG_INFO);
	pv_log_set_chiaki_console(pv_global.verbose);
	pv_log_set_trace(pv_global.trace);

	if(want_version || want_help || want_license || nrest == 0)
	{
		pv_log_init(NULL, 0);
		if(want_version)
			print_version();
		else if(want_license)
			print_license();
		else
			print_usage();
		bool pause = nrest == 0 && !want_version && !want_help && !want_license && pv_console_owned_by_us();
		if(pause)
		{
			pv_out("%s", "");
			pv_out("ps5view ist ein Kommandozeilenprogramm: bitte aus der Eingabeaufforderung oder PowerShell starten.");
			pv_console_pause();
		}
		free(rest);
		return PV_EXIT_OK;
	}

	const char *cmd_name = rest[0];
	const Command *cmd = NULL;
	for(size_t i = 0; i < PV_ARRAY_SIZE(commands); i++)
		if(strcmp(commands[i].name, cmd_name) == 0)
			cmd = &commands[i];
	if(!cmd)
	{
		pv_log_init(NULL, 0);
		LOGE("main", "Unbekannter Befehl \"%s\". Übersicht: ps5view --help", cmd_name);
		free(rest);
		return PV_EXIT_USAGE;
	}

	char *log_path = NULL;
	if(!no_log_file)
		log_path = log_file ? pv_strdup(log_file) : default_log_path();
	pv_log_init(log_path, log_file ? 0 : 20);
	pv_interrupt_install();

	char os[128];
	pv_os_description(os, sizeof(os));
	LOGD("main", "ps5view %s (git %s), libchiaki %s, %s", PV_VERSION, PV_GIT_COMMIT, PV_CHIAKI_COMMIT, os);
	log_command_line(argc, argv);
	if(log_path)
		LOGD("main", "Logdatei: %s", log_path);

	ChiakiErrorCode lerr = chiaki_lib_init();
	int rc;
	if(lerr != CHIAKI_ERR_SUCCESS)
	{
		LOGE("main", "libchiaki konnte nicht initialisiert werden: %s", chiaki_error_string(lerr));
		rc = PV_EXIT_INTERNAL;
	}
	else
		rc = cmd->fn(nrest - 1, rest + 1);

	if(rc == PV_EXIT_INTERRUPTED || (pv_interrupted() && rc == PV_EXIT_OK))
		LOGI("main", "Abgebrochen.");
	if(rc != PV_EXIT_OK && rc != PV_EXIT_USAGE && rc != PV_EXIT_INTERRUPTED && log_path)
		LOGI("main", "Details stehen in der Logdatei: %s", log_path);
	LOGD("main", "Beendet mit Code %d", rc);
	pv_log_close();
	pv_shutdown_complete();
	if(rc != PV_EXIT_OK && rc != PV_EXIT_INTERRUPTED && pv_console_owned_by_us())
		pv_console_pause();
	free(log_path);
	free(rest);
	return rc;
}
