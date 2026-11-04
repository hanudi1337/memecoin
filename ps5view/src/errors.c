// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "errors.h"

#include "app.h"
#include "log.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define HINT_RP_SETTINGS "Einstellungen > System > Remote Play"
#define HINT_LINK_DEVICE "Einstellungen > System > Remote Play > Gerät verknüpfen"

// RP-Application-Reason codes. The first six are defined in chiaki-ng
// (lib/include/chiaki/session.h); 0x80108b12 and 0x80108b13 are not known to
// chiaki-ng, their meaning comes from other open-source Remote Play clients
// (openps-rs: crates/openps-rp/src/regist.rs, ps5upload: rp_stream_api.rs) and
// is not officially documented by Sony.
const PvAppReason pv_app_reasons[] = {
	{ 0x80108b02,
		"Die Konsole akzeptiert die PSN-Account-ID nicht (0x80108b02).",
		"- Account-ID neu ermitteln: ps5view account-id\n"
		"- Es muss das PSN-Konto sein, mit dem du an der PS5 angemeldet bist (bei mehreren Benutzern genau dieser Benutzer).\n"
		"- Das Konto muss an der PS5 mindestens einmal online angemeldet gewesen sein.",
		PV_EXIT_AUTH, PV_RETRY_NO },
	{ 0x80108b09,
		"Registrierung abgelehnt: Die PIN ist falsch oder abgelaufen (0x80108b09).",
		"- An der PS5: " HINT_LINK_DEVICE ". Die 8-stellige PIN gilt nur, solange dieser Bildschirm offen ist (wenige Minuten).\n"
		"- Neue PIN anzeigen lassen und sofort erneut registrieren: ps5view register --host <ip> --account-id <id> --pin <neue PIN>\n"
		"- Account-ID prüfen: Sie muss zu dem PSN-Konto gehören, das an der PS5 angemeldet ist.",
		PV_EXIT_AUTH, PV_RETRY_NO },
	{ 0x80108b10,
		"Remote Play ist bereits durch ein anderes Gerät belegt (0x80108b10).",
		"- Andere Remote-Play-Verbindung beenden: PS App am Handy, PS Remote Play oder chiaki-ng auf einem anderen PC, PlayStation Portal.\n"
		"- Nach einem Absturz hält die PS5 die alte Sitzung noch kurz fest; ps5view versucht es deshalb mit längeren Pausen erneut.",
		PV_EXIT_CONSOLE, PV_RETRY_SLOW },
	{ 0x80108b11,
		"Die Remote-Play-Version passt nicht zur Konsole (0x80108b11).",
		"- Systemsoftware der PS5 aktualisieren.\n"
		"- Prüfen, ob es eine neuere ps5view-/chiaki-ng-Version gibt.",
		PV_EXIT_CONSOLE, PV_RETRY_NO },
	{ 0x80108b12,
		"Die PS5 lehnt die Sitzung ab (0x80108b12): Remote Play ist an der Konsole sehr wahrscheinlich deaktiviert.",
		"- An der PS5: " HINT_RP_SETTINGS " > \"Remote Play aktivieren\" einschalten.\n"
		"- Das registrierte PSN-Konto muss an der PS5 angemeldet sein (kein reines Offline-Konto).\n"
		"- Für den Start aus dem Ruhemodus: Einstellungen > System > Energiesparen > Im Ruhemodus verfügbare Funktionen >\n"
		"  \"Mit dem Internet verbunden bleiben\" und \"Einschalten der PS5 über das Netzwerk ermöglichen\" aktivieren.\n"
		"- Danach die PS5 einmal komplett neu starten (nicht nur Ruhemodus).\n"
		"- Hinweis: Sony dokumentiert diesen Code nicht; die Zuordnung stammt aus den Open-Source-Clients openps-rs und ps5upload.",
		PV_EXIT_CONSOLE, PV_RETRY_NO },
	{ 0x80108b13,
		"Die Konsole verweigert den Zugriff (0x80108b13, \"not allowed\").",
		"- Prüfen, ob das PSN-Konto Remote Play nutzen darf (z. B. Kinder-/Familienkonto mit Einschränkungen).\n"
		"- Gerät neu verknüpfen (" HINT_LINK_DEVICE ") und neu registrieren.\n"
		"- Hinweis: Bedeutung nicht offiziell dokumentiert (Quelle: openps-rs).",
		PV_EXIT_AUTH, PV_RETRY_NO },
	{ 0x80108b15,
		"Remote Play ist auf der Konsole abgestürzt (0x80108b15).",
		"- Die PS5 komplett neu starten (nicht nur Ruhemodus) und erneut versuchen.",
		PV_EXIT_CONSOLE, PV_RETRY_SLOW },
	{ 0x80108bff,
		"Die Konsole akzeptiert die Zugangsdaten nicht (0x80108bff).",
		"- Bei der Registrierung: neue PIN anfordern (" HINT_LINK_DEVICE ") und die Account-ID prüfen.\n"
		"- Beim Streamen: Die Registrierung wurde vermutlich an der PS5 entfernt - dann neu registrieren.",
		PV_EXIT_AUTH, PV_RETRY_NO },
};
const size_t pv_app_reasons_count = PV_ARRAY_SIZE(pv_app_reasons);

#define HINT_NETWORK_BASICS \
	"- IP-Adresse prüfen: ps5view discover (nach einem Router-Neustart kann sie sich ändern; am besten im Router eine feste IP für die PS5 vergeben).\n" \
	"- PC und PS5 müssen im selben Heimnetz sein (kein Gast-WLAN, keine WLAN-Client-Isolation).\n" \
	"- VPN am PC trennen; Firewall/Sicherheitssoftware muss ps5view erlauben."

#define HINT_FIREWALL_UDP \
	"- Windows-Firewall: ps5view.exe für \"Private Netzwerke\" zulassen\n" \
	"  (Windows-Sicherheit > Firewall- & Netzwerkschutz > App durch die Firewall zulassen).\n" \
	"- Das Netzwerkprofil der LAN/WLAN-Verbindung muss \"Privat\" sein, nicht \"Öffentlich\".\n" \
	"- VPN- oder Sicherheitssoftware testweise deaktivieren.\n" \
	"- Der PC muss UDP-Pakete von der PS5 empfangen dürfen (Ports 9296 und 9297)."

const PvAppReason *pv_app_reason_lookup(uint32_t code)
{
	for(size_t i = 0; i < pv_app_reasons_count; i++)
		if(pv_app_reasons[i].code == code)
			return &pv_app_reasons[i];
	return NULL;
}

void pv_failure_clear(PvFailure *f)
{
	memset(f, 0, sizeof(*f));
	f->exit_code = PV_EXIT_OK;
	f->retry = PV_RETRY_NO;
}

void pv_failure_set(PvFailure *f, int exit_code, PvRetryClass retry, const char *hint, const char *fmt, ...)
{
	f->failed = true;
	f->exit_code = exit_code;
	f->retry = retry;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(f->message, sizeof(f->message), fmt, ap);
	va_end(ap);
	pv_strlcpy(f->hint, hint ? hint : "", sizeof(f->hint));
}

void pv_failure_from_app_reason(PvFailure *f, uint32_t code)
{
	const PvAppReason *r = pv_app_reason_lookup(code);
	if(r)
	{
		pv_failure_set(f, r->exit_code, r->retry, r->hint, "%s", r->message);
		return;
	}
	pv_failure_set(f, PV_EXIT_CONSOLE, PV_RETRY_FAST,
			"- PS5 neu starten und erneut versuchen.\n"
			"- Remote Play an der PS5 prüfen (" HINT_RP_SETTINGS ").\n"
			"- Die Logdatei enthält Details; bitte bei Problemberichten mitschicken.",
			"Die Konsole lehnt die Anfrage mit einem unbekannten Code ab (0x%08x).", (unsigned)code);
}

static void set_detail(PvFailure *f, const PvDiagInfo *diag)
{
	if(diag && diag->last_error[0])
		snprintf(f->detail, sizeof(f->detail), "Letzte libchiaki-Fehlermeldung: %s", diag->last_error);
}

void pv_failure_from_session(PvFailure *f, ChiakiQuitReason reason, const char *reason_str,
		const PvDiagInfo *diag, const char *host)
{
	pv_failure_clear(f);
	const char *h = host ? host : "?";

	switch(reason)
	{
		case CHIAKI_QUIT_REASON_STOPPED:
			return; // regular stop requested by us
		case CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN:
			// not an error: console went to rest mode / was switched off
			pv_failure_set(f, PV_EXIT_OK, PV_RETRY_NO, "",
					"Die PS5 hat die Verbindung beendet (Ruhemodus oder Ausschalten).");
			f->failed = false;
			return;
		default:
			break;
	}

	if(diag && diag->has_app_reason)
		pv_failure_from_app_reason(f, diag->app_reason);
	else if(reason == CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE)
		pv_failure_from_app_reason(f, 0x80108b10);
	else if(reason == CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_CRASH)
		pv_failure_from_app_reason(f, 0x80108b15);
	else if(reason == CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_VERSION_MISMATCH)
		pv_failure_from_app_reason(f, 0x80108b11);
	else if(reason == CHIAKI_QUIT_REASON_SESSION_REQUEST_CONNECTION_REFUSED || (diag && diag->session_connect_refused))
		pv_failure_set(f, PV_EXIT_NETWORK, PV_RETRY_FAST,
				"- Die PS5 ist vermutlich im Ruhemodus oder startet gerade: mit --wake aufwecken oder PS-Taste drücken.\n"
				"- Remote Play an der PS5 aktiviert? (" HINT_RP_SETTINGS ")\n"
				HINT_NETWORK_BASICS,
				"Die PS5 (%s) nimmt keine Remote-Play-Verbindungen an (TCP-Port 9295 abgelehnt).", h);
	else if(reason == CHIAKI_QUIT_REASON_SESSION_REQUEST_UNKNOWN && diag && diag->session_connect_failed)
		pv_failure_set(f, PV_EXIT_NETWORK, PV_RETRY_FAST,
				"- Ist die PS5 eingeschaltet oder im Ruhemodus (nicht komplett aus)? Aus dem Ruhemodus mit --wake.\n"
				HINT_NETWORK_BASICS,
				"Keine Verbindung zur PS5 unter %s:9295%s.", h, diag->session_connect_timeout ? " (Zeitüberschreitung)" : "");
	else if(reason == CHIAKI_QUIT_REASON_SESSION_REQUEST_UNKNOWN && diag && diag->session_no_response)
		pv_failure_set(f, PV_EXIT_NETWORK, PV_RETRY_FAST,
				"- Die PS5 startet eventuell gerade noch; ps5view versucht es erneut.\n" HINT_NETWORK_BASICS,
				"Die PS5 (%s) hat die Sitzungsanfrage nicht beantwortet.", h);
	else if(reason == CHIAKI_QUIT_REASON_SESSION_REQUEST_UNKNOWN)
		pv_failure_set(f, PV_EXIT_NETWORK, PV_RETRY_FAST, HINT_NETWORK_BASICS,
				"Die Sitzungsanfrage an die PS5 (%s) ist fehlgeschlagen.", h);
	else if((reason == CHIAKI_QUIT_REASON_CTRL_UNKNOWN || reason == CHIAKI_QUIT_REASON_CTRL_CONNECT_FAILED
				|| reason == CHIAKI_QUIT_REASON_CTRL_CONNECTION_REFUSED) && diag && diag->ctrl_http_code == 403)
		pv_failure_set(f, PV_EXIT_AUTH, PV_RETRY_NO,
				"- Die gespeicherten Schlüssel passen nicht mehr zur PS5 (Registrierung entfernt oder durch neue ersetzt).\n"
				"- Neu verknüpfen: " HINT_LINK_DEVICE ", dann ps5view register ... --name <gleicher Profilname>.",
				"Die PS5 lehnt die Steuerverbindung ab (HTTP 403): Die Registrierung ist ungültig.");
	else if(reason == CHIAKI_QUIT_REASON_CTRL_CONNECTION_REFUSED || reason == CHIAKI_QUIT_REASON_CTRL_CONNECT_FAILED)
		pv_failure_set(f, PV_EXIT_NETWORK, PV_RETRY_FAST, HINT_NETWORK_BASICS,
				"Die Steuerverbindung zur PS5 (%s, TCP 9295) ist fehlgeschlagen.", h);
	else if(reason == CHIAKI_QUIT_REASON_CTRL_UNKNOWN)
		pv_failure_set(f, PV_EXIT_NETWORK, PV_RETRY_FAST,
				"- Netzwerkverbindung prüfen (WLAN-Empfang, LAN-Kabel).\n" HINT_NETWORK_BASICS,
				"Die Steuerverbindung zur PS5 wurde unerwartet beendet.");
	else if(reason == CHIAKI_QUIT_REASON_STREAM_CONNECTION_UNKNOWN && diag && diag->stream_takion_failed)
		pv_failure_set(f, PV_EXIT_NETWORK, PV_RETRY_FAST, HINT_FIREWALL_UDP,
				"Die Video-/Audioverbindung (UDP 9296) kam nicht zustande - vermutlich blockiert eine Firewall UDP.");
	else if(reason == CHIAKI_QUIT_REASON_STREAM_CONNECTION_UNKNOWN && diag && diag->stream_bang_timeout)
		pv_failure_set(f, PV_EXIT_NETWORK, PV_RETRY_FAST, HINT_FIREWALL_UDP,
				"Die PS5 hat den Stream-Handshake nicht beantwortet.");
	else if(reason == CHIAKI_QUIT_REASON_STREAM_CONNECTION_UNKNOWN)
		pv_failure_set(f, PV_EXIT_NETWORK, PV_RETRY_FAST,
				"- Bei WLAN: Empfang prüfen oder LAN-Kabel verwenden; Bitrate senken (z. B. --bitrate 8000).\n"
				HINT_FIREWALL_UDP,
				"Die Streamverbindung zur PS5 ist abgebrochen.");
	else if(reason == CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_DISCONNECTED)
		pv_failure_set(f, PV_EXIT_CONSOLE, PV_RETRY_FAST,
				"- Wurde Remote Play an der PS5 beendet oder hat sich ein anderes Gerät verbunden?\n"
				"- Bei instabilem WLAN: Bitrate senken (--bitrate 8000) oder LAN-Kabel verwenden.",
				"Die PS5 hat den Stream beendet: \"%s\".", reason_str && *reason_str ? reason_str : "ohne Begründung");
	else if(reason == CHIAKI_QUIT_REASON_PSN_REGIST_FAILED)
		pv_failure_set(f, PV_EXIT_AUTH, PV_RETRY_NO, "", "PSN-Registrierung fehlgeschlagen.");
	else
		pv_failure_set(f, PV_EXIT_INTERNAL, PV_RETRY_FAST,
				"- Logdatei prüfen; mit --verbose erneut starten, um alle libchiaki-Meldungen zu sehen.",
				"Die Verbindung ist fehlgeschlagen (%s).", chiaki_quit_reason_string(reason));

	if(diag && diag->senkusha_failed && f->exit_code == PV_EXIT_NETWORK)
		strncat(f->hint, "\n- Auch der Netzwerktest der PS5 (UDP 9297) ist fehlgeschlagen - das spricht für eine Firewall-Blockade.",
				sizeof(f->hint) - strlen(f->hint) - 1);
	set_detail(f, diag);
}

void pv_failure_from_regist(PvFailure *f, const PvDiagInfo *diag, const char *host)
{
	pv_failure_clear(f);
	const char *h = host ? host : "?";
	if(diag->has_app_reason)
		pv_failure_from_app_reason(f, diag->app_reason);
	else if(diag->regist_http_code && diag->regist_http_code != 200)
		pv_failure_set(f, PV_EXIT_AUTH, PV_RETRY_NO,
				"- PIN neu anfordern (" HINT_LINK_DEVICE ") und sofort verwenden.\n"
				"- Account-ID prüfen (ps5view account-id): Sie muss zum an der PS5 angemeldeten PSN-Konto gehören.",
				"Die PS5 hat die Registrierung abgelehnt (HTTP %d).", diag->regist_http_code);
	else if(diag->regist_search_refused)
		pv_failure_set(f, PV_EXIT_CONSOLE, PV_RETRY_NO,
				"- An der PS5 " HINT_LINK_DEVICE " öffnen und den Bildschirm mit der PIN offen lassen, bis ps5view fertig ist.\n"
				"- Die gerade angezeigte PIN verwenden (sie ändert sich bei jedem Öffnen).\n"
				"- Stimmt die IP-Adresse? (ps5view discover)",
				"Die PS5 (%s) hat die Registrierungsanfrage abgelehnt: Der Bildschirm \"Gerät verknüpfen\" ist nicht geöffnet.", h);
	else if(diag->regist_search_timeout)
		pv_failure_set(f, PV_EXIT_NETWORK, PV_RETRY_NO,
				"- Ist an der PS5 der Bildschirm \"Gerät verknüpfen\" mit der PIN geöffnet?\n"
				HINT_NETWORK_BASICS "\n"
				"- Firewall: UDP-Port 9295 zur PS5 und die Antwort der PS5 müssen erlaubt sein.",
				"Keine Antwort der PS5 (%s) auf die Registrierungsanfrage (UDP 9295).", h);
	else if(diag->regist_resolve_failed)
		pv_failure_set(f, PV_EXIT_USAGE, PV_RETRY_NO,
				"- Die IPv4-Adresse der PS5 angeben, z. B. --host 192.168.178.88 (ps5view discover zeigt sie an).",
				"Die Adresse \"%s\" ist ungültig oder kann nicht aufgelöst werden.", h);
	else if(diag->regist_connect_failed)
		pv_failure_set(f, PV_EXIT_NETWORK, PV_RETRY_NO, HINT_NETWORK_BASICS,
				"Die Verbindung zur PS5 (%s, TCP 9295) für die Registrierung ist fehlgeschlagen.", h);
	else
		pv_failure_set(f, PV_EXIT_NETWORK, PV_RETRY_NO,
				"- Bildschirm \"Gerät verknüpfen\" an der PS5 offen? PIN aktuell?\n" HINT_NETWORK_BASICS,
				"Die Registrierung ist fehlgeschlagen.");
	set_detail(f, diag);
}

void pv_failure_print(const PvFailure *f)
{
	if(!f->message[0])
		return;
	if(f->failed)
		LOGE("error", "%s", f->message);
	else
		LOGI("error", "%s", f->message);
	if(f->hint[0])
		LOGI("hint", "Was du tun kannst:\n%s", f->hint);
	if(f->detail[0])
		LOGI("hint", "%s", f->detail);
}

const char *pv_retry_class_name(PvRetryClass r)
{
	switch(r)
	{
		case PV_RETRY_NO: return "kein Neuversuch";
		case PV_RETRY_FAST: return "Neuversuch";
		case PV_RETRY_SLOW: return "Neuversuch mit langer Pause";
	}
	return "?";
}
