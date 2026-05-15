/*
 * Kongoria Voice Mumble Plugin
 * SPDX-License-Identifier: MIT
 *
 * Native Mumble audio plugin that connects to the Kongoria falloff server and
 * applies per-speaker gain/pan updates to received voice packets.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../third_party/mumble/MumblePlugin.h"

#define EXILE_VERSION_MAJOR 0
#define EXILE_VERSION_MINOR 1
#define EXILE_VERSION_PATCH 0

#define EXILE_MAX_PATH 1024
#define EXILE_MAX_HASH 128
#define EXILE_MAX_USERS 512
#define EXILE_MAX_STATES 512
#define EXILE_MAX_LINE 8192

typedef struct ExileConfig {
	char server_host[256];
	int server_port;
	char api_key[256];
	bool enabled;
	bool debug_log;
	float smoothing;
	float pan_smoothing;
	int reconnect_sec;
} ExileConfig;

typedef struct UserHashEntry {
	mumble_userid_t user_id;
	char hash[EXILE_MAX_HASH];
} UserHashEntry;

typedef struct AudioState {
	char hash[EXILE_MAX_HASH];
	float gain;
	float pan;
} AudioState;

static mumble_plugin_id_t g_plugin_id = 0;
static mumble_connection_t g_connection = -1;
static MumbleAPI g_api;
static bool g_api_ready = false;
static bool g_initialized = false;
static bool g_ws_started = false;
static volatile LONG g_worker_running = 0;
static HANDLE g_worker_thread = NULL;
static CRITICAL_SECTION g_lock;

static ExileConfig g_config;
static char g_plugin_dir[EXILE_MAX_PATH] = { 0 };
static char g_config_path[EXILE_MAX_PATH] = { 0 };
static char g_log_path[EXILE_MAX_PATH] = { 0 };
static char g_local_hash[EXILE_MAX_HASH] = { 0 };

static UserHashEntry g_user_hashes[EXILE_MAX_USERS];
static size_t g_user_hash_count = 0;
static AudioState g_audio_states[EXILE_MAX_STATES];
static size_t g_audio_state_count = 0;

extern IMAGE_DOS_HEADER __ImageBase;

static struct MumbleStringWrapper wrap_static(const char *text) {
	struct MumbleStringWrapper wrapper;
	wrapper.data = text;
	wrapper.size = strlen(text);
	wrapper.needsReleasing = false;
	return wrapper;
}

static void safe_copy(char *dst, size_t dst_size, const char *src) {
	if (!dst || dst_size == 0) {
		return;
	}
	if (!src) {
		dst[0] = '\0';
		return;
	}
	snprintf(dst, dst_size, "%s", src);
}

static char *trim(char *text) {
	while (*text && isspace((unsigned char) *text)) {
		text++;
	}
	if (!*text) {
		return text;
	}
	char *end = text + strlen(text) - 1;
	while (end > text && isspace((unsigned char) *end)) {
		*end = '\0';
		end--;
	}
	return text;
}

static float clampf(float value, float min_value, float max_value) {
	if (value < min_value) {
		return min_value;
	}
	if (value > max_value) {
		return max_value;
	}
	return value;
}

static void mumble_log(const char *message) {
	if (g_api_ready && g_api.log) {
		g_api.log(g_plugin_id, message);
	}
}

static void file_log(const char *fmt, ...) {
	if (!g_config.debug_log || !g_log_path[0]) {
		return;
	}

	FILE *file = fopen(g_log_path, "ab");
	if (!file) {
		return;
	}

	time_t now = time(NULL);
	struct tm local_time;
	localtime_s(&local_time, &now);
	fprintf(file, "[%04d-%02d-%02d %02d:%02d:%02d] ",
			local_time.tm_year + 1900, local_time.tm_mon + 1, local_time.tm_mday,
			local_time.tm_hour, local_time.tm_min, local_time.tm_sec);

	va_list args;
	va_start(args, fmt);
	vfprintf(file, fmt, args);
	va_end(args);

	fprintf(file, "\r\n");
	fclose(file);
}

static void log_both(const char *fmt, ...) {
	char buffer[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	char mumble_message[1200];
	snprintf(mumble_message, sizeof(mumble_message), "[kongoria_voice] %s", buffer);
	mumble_log(mumble_message);
	file_log("%s", buffer);
}

static void set_default_config(void) {
	safe_copy(g_config.server_host, sizeof(g_config.server_host), "127.0.0.1");
	g_config.server_port = 8890;
	safe_copy(g_config.api_key, sizeof(g_config.api_key), "CHANGE_ME_SHARED_KEY");
	g_config.enabled = true;
	g_config.debug_log = true;
	g_config.smoothing = 0.80f;
	g_config.pan_smoothing = 0.95f;
	g_config.reconnect_sec = 8;
}

static bool parse_bool(const char *value) {
	return _stricmp(value, "1") == 0 || _stricmp(value, "true") == 0 || _stricmp(value, "yes") == 0
		   || _stricmp(value, "on") == 0;
}

static void apply_config_value(const char *key, const char *value) {
	if (_stricmp(key, "server_host") == 0) {
		safe_copy(g_config.server_host, sizeof(g_config.server_host), value);
	} else if (_stricmp(key, "server_port") == 0) {
		g_config.server_port = atoi(value);
	} else if (_stricmp(key, "api_key") == 0) {
		safe_copy(g_config.api_key, sizeof(g_config.api_key), value);
	} else if (_stricmp(key, "enabled") == 0) {
		g_config.enabled = parse_bool(value);
	} else if (_stricmp(key, "debug_log") == 0) {
		g_config.debug_log = parse_bool(value);
	} else if (_stricmp(key, "smoothing") == 0) {
		g_config.smoothing = clampf((float) atof(value), 0.0f, 1.0f);
	} else if (_stricmp(key, "pan_smoothing") == 0) {
		g_config.pan_smoothing = clampf((float) atof(value), 0.0f, 1.0f);
	} else if (_stricmp(key, "reconnect_sec") == 0) {
		g_config.reconnect_sec = atoi(value);
		if (g_config.reconnect_sec < 1) {
			g_config.reconnect_sec = 1;
		}
	}
}

static void discover_plugin_paths(void) {
	char dll_path[EXILE_MAX_PATH];
	DWORD len = GetModuleFileNameA((HINSTANCE) &__ImageBase, dll_path, sizeof(dll_path));
	if (len == 0 || len >= sizeof(dll_path)) {
		safe_copy(g_plugin_dir, sizeof(g_plugin_dir), ".");
	} else {
		char *slash = strrchr(dll_path, '\\');
		if (slash) {
			*slash = '\0';
			safe_copy(g_plugin_dir, sizeof(g_plugin_dir), dll_path);
		} else {
			safe_copy(g_plugin_dir, sizeof(g_plugin_dir), ".");
		}
	}

	snprintf(g_config_path, sizeof(g_config_path), "%s\\kongoria_voice.ini", g_plugin_dir);
	snprintf(g_log_path, sizeof(g_log_path), "%s\\kongoria_voice.log", g_plugin_dir);
}

static void load_config(void) {
	set_default_config();
	discover_plugin_paths();

	FILE *file = fopen(g_config_path, "rb");
	if (file) {
		char line[1024];
		while (fgets(line, sizeof(line), file)) {
			char *text = trim(line);
			if (!text[0] || text[0] == '#' || text[0] == ';') {
				continue;
			}
			char *equals = strchr(text, '=');
			if (!equals) {
				continue;
			}
			*equals = '\0';
			char *key = trim(text);
			char *value = trim(equals + 1);
			apply_config_value(key, value);
		}
		fclose(file);
	}

	char env_host[256];
	DWORD env_len = GetEnvironmentVariableA("KONGORIA_VOICE_HOST", env_host, sizeof(env_host));
	if (env_len > 0 && env_len < sizeof(env_host)) {
		safe_copy(g_config.server_host, sizeof(g_config.server_host), env_host);
	}

	char env_port[32];
	env_len = GetEnvironmentVariableA("KONGORIA_VOICE_PORT", env_port, sizeof(env_port));
	if (env_len > 0 && env_len < sizeof(env_port)) {
		g_config.server_port = atoi(env_port);
	}
}

static void clear_audio_states_locked(void) {
	g_audio_state_count = 0;
}

static void upsert_audio_state_locked(const char *hash, float gain, float pan) {
	if (!hash || !hash[0]) {
		return;
	}

	for (size_t i = 0; i < g_audio_state_count; i++) {
		if (_stricmp(g_audio_states[i].hash, hash) == 0) {
			g_audio_states[i].gain = gain;
			g_audio_states[i].pan = pan;
			return;
		}
	}

	if (g_audio_state_count >= EXILE_MAX_STATES) {
		return;
	}

	AudioState *state = &g_audio_states[g_audio_state_count++];
	safe_copy(state->hash, sizeof(state->hash), hash);
	state->gain = gain;
	state->pan = pan;
}

static bool find_hash_for_user_locked(mumble_userid_t user_id, char *hash, size_t hash_size) {
	for (size_t i = 0; i < g_user_hash_count; i++) {
		if (g_user_hashes[i].user_id == user_id) {
			safe_copy(hash, hash_size, g_user_hashes[i].hash);
			return true;
		}
	}
	return false;
}

static bool find_audio_state_locked(const char *hash, float *gain, float *pan) {
	for (size_t i = 0; i < g_audio_state_count; i++) {
		if (_stricmp(g_audio_states[i].hash, hash) == 0) {
			*gain = g_audio_states[i].gain;
			*pan = g_audio_states[i].pan;
			return true;
		}
	}
	return false;
}

static void refresh_user_hashes(void) {
	if (!g_api_ready || !g_api.getAllUsers || !g_api.getUserHash || !g_api.freeMemory || g_connection < 0) {
		return;
	}

	mumble_userid_t *users = NULL;
	size_t user_count = 0;
	if (g_api.getAllUsers(g_plugin_id, g_connection, &users, &user_count) != MUMBLE_STATUS_OK || !users) {
		return;
	}

	UserHashEntry next_entries[EXILE_MAX_USERS];
	size_t next_count = 0;

	for (size_t i = 0; i < user_count && next_count < EXILE_MAX_USERS; i++) {
		const char *hash = NULL;
		if (g_api.getUserHash(g_plugin_id, g_connection, users[i], &hash) == MUMBLE_STATUS_OK && hash && hash[0]) {
			next_entries[next_count].user_id = users[i];
			safe_copy(next_entries[next_count].hash, sizeof(next_entries[next_count].hash), hash);
			next_count++;
			g_api.freeMemory(g_plugin_id, hash);
		}
	}

	g_api.freeMemory(g_plugin_id, users);

	EnterCriticalSection(&g_lock);
	memcpy(g_user_hashes, next_entries, sizeof(UserHashEntry) * next_count);
	g_user_hash_count = next_count;
	LeaveCriticalSection(&g_lock);

	file_log("refreshed %zu Mumble user hashes", next_count);
}

static void update_local_hash(void) {
	if (!g_api_ready || !g_api.getLocalUserID || !g_api.getUserHash || !g_api.freeMemory || g_connection < 0) {
		return;
	}

	mumble_userid_t local_user = 0;
	if (g_api.getLocalUserID(g_plugin_id, g_connection, &local_user) != MUMBLE_STATUS_OK) {
		log_both("failed to read local Mumble user id");
		return;
	}

	const char *hash = NULL;
	if (g_api.getUserHash(g_plugin_id, g_connection, local_user, &hash) == MUMBLE_STATUS_OK && hash && hash[0]) {
		EnterCriticalSection(&g_lock);
		safe_copy(g_local_hash, sizeof(g_local_hash), hash);
		LeaveCriticalSection(&g_lock);

		log_both("local Mumble hash=%.*s", 8, hash);
		g_api.freeMemory(g_plugin_id, hash);
		return;
	}

	if (hash) {
		g_api.freeMemory(g_plugin_id, hash);
	}

	EnterCriticalSection(&g_lock);
	g_local_hash[0] = '\0';
	LeaveCriticalSection(&g_lock);

	log_both("Mumble certificate hash is not yet available. "
			 "If this persists, generate a certificate via Configure -> Certificate Wizard "
			 "and reconnect. Spatial voice will activate as soon as the hash is known.");
}

static bool read_socket_line(SOCKET sock, char *line, size_t line_size) {
	size_t len = 0;
	while (len + 1 < line_size) {
		char c = 0;
		int got = recv(sock, &c, 1, 0);
		if (got == 1) {
			if (c == '\n') {
				line[len] = '\0';
				return true;
			}
			if (c != '\r') {
				line[len++] = c;
			}
			continue;
		}
		if (got == 0) {
			return false;
		}
		int err = WSAGetLastError();
		if (err == WSAETIMEDOUT) {
			line[len] = '\0';
			return len > 0;
		}
		return false;
	}
	line[len] = '\0';
	return true;
}

static bool connect_falloff(SOCKET *out_sock) {
	char port[32];
	snprintf(port, sizeof(port), "%d", g_config.server_port);

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	struct addrinfo *results = NULL;
	if (getaddrinfo(g_config.server_host, port, &hints, &results) != 0) {
		file_log("falloff resolve failed host=%s port=%s", g_config.server_host, port);
		return false;
	}

	bool connected = false;
	for (struct addrinfo *it = results; it != NULL; it = it->ai_next) {
		SOCKET sock = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
		if (sock == INVALID_SOCKET) {
			continue;
		}

		DWORD timeout_ms = 5000;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *) &timeout_ms, sizeof(timeout_ms));
		setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *) &timeout_ms, sizeof(timeout_ms));

		if (connect(sock, it->ai_addr, (int) it->ai_addrlen) == 0) {
			*out_sock = sock;
			connected = true;
			break;
		}

		closesocket(sock);
	}

	freeaddrinfo(results);
	return connected;
}

static void parse_update_line(const char *line) {
	if (strncmp(line, "UPDATE2", 7) != 0 && strncmp(line, "UPDATE", 6) != 0) {
		return;
	}

	bool has_pan = strncmp(line, "UPDATE2", 7) == 0;
	const char *payload = line + (has_pan ? 7 : 6);
	while (*payload && isspace((unsigned char) *payload)) {
		payload++;
	}

	EnterCriticalSection(&g_lock);
	clear_audio_states_locked();

	if (*payload) {
		char buffer[EXILE_MAX_LINE];
		safe_copy(buffer, sizeof(buffer), payload);
		char *context = NULL;
		char *entry = strtok_s(buffer, ",", &context);
		while (entry) {
			char hash[EXILE_MAX_HASH] = { 0 };
			float gain = 0.0f;
			float pan = 0.0f;
			char *trimmed = trim(entry);
			if (has_pan) {
				if (sscanf(trimmed, "%127s %f %f", hash, &gain, &pan) >= 2) {
					upsert_audio_state_locked(hash, clampf(gain, 0.0f, 1.0f), clampf(pan, -1.0f, 1.0f));
				}
			} else {
				if (sscanf(trimmed, "%127s %f", hash, &gain) == 2) {
					upsert_audio_state_locked(hash, clampf(gain, 0.0f, 1.0f), 0.0f);
				}
			}
			entry = strtok_s(NULL, ",", &context);
		}
	}

	size_t state_count = g_audio_state_count;
	LeaveCriticalSection(&g_lock);
	file_log("received %s with %zu speakers", has_pan ? "UPDATE2" : "UPDATE", state_count);
}

static DWORD WINAPI falloff_worker(LPVOID unused) {
	(void) unused;

	while (InterlockedCompareExchange(&g_worker_running, 1, 1) == 1) {
		char local_hash[EXILE_MAX_HASH];
		EnterCriticalSection(&g_lock);
		safe_copy(local_hash, sizeof(local_hash), g_local_hash);
		LeaveCriticalSection(&g_lock);

		if (!g_config.enabled || !local_hash[0] || !g_config.api_key[0]) {
			Sleep((DWORD) g_config.reconnect_sec * 1000);
			continue;
		}

		SOCKET sock = INVALID_SOCKET;
		file_log("falloff_connect_attempt host=%s port=%d", g_config.server_host, g_config.server_port);
		if (!connect_falloff(&sock)) {
			file_log("falloff_connect_fail host=%s port=%d", g_config.server_host, g_config.server_port);
			Sleep((DWORD) g_config.reconnect_sec * 1000);
			continue;
		}

		char hello[1024];
		snprintf(hello, sizeof(hello),
				 "HELLO %s %s {\"version\":\"1.0\",\"client\":\"kongoria_voice\",\"features\":[\"spatial\"]}\n",
				 g_config.api_key, local_hash);
		if (send(sock, hello, (int) strlen(hello), 0) == SOCKET_ERROR) {
			file_log("falloff_hello_send_fail wsa=%d", WSAGetLastError());
			closesocket(sock);
			Sleep((DWORD) g_config.reconnect_sec * 1000);
			continue;
		}

		char line[EXILE_MAX_LINE];
		if (!read_socket_line(sock, line, sizeof(line)) || strncmp(line, "OK", 2) != 0) {
			file_log("falloff_hello_rejected reply=%s", line);
			closesocket(sock);
			Sleep((DWORD) g_config.reconnect_sec * 1000);
			continue;
		}

		log_both("connected to falloff %s:%d", g_config.server_host, g_config.server_port);
		DWORD last_ping = GetTickCount();

		while (InterlockedCompareExchange(&g_worker_running, 1, 1) == 1) {
			if (read_socket_line(sock, line, sizeof(line))) {
				if (line[0]) {
					if (strcmp(line, "PING") == 0) {
						send(sock, "OK\n", 3, 0);
					} else if (strncmp(line, "UPDATE", 6) == 0) {
						parse_update_line(line);
					}
				}
			} else {
				int err = WSAGetLastError();
				if (err != WSAETIMEDOUT) {
					file_log("falloff_disconnected wsa=%d", err);
					break;
				}
			}

			DWORD now = GetTickCount();
			if (now - last_ping > 20000) {
				if (send(sock, "PING\n", 5, 0) == SOCKET_ERROR) {
					file_log("falloff_ping_send_fail wsa=%d", WSAGetLastError());
					break;
				}
				last_ping = now;
			}
		}

		closesocket(sock);
		EnterCriticalSection(&g_lock);
		clear_audio_states_locked();
		LeaveCriticalSection(&g_lock);

		if (InterlockedCompareExchange(&g_worker_running, 1, 1) == 1) {
			Sleep((DWORD) g_config.reconnect_sec * 1000);
		}
	}

	return 0;
}

static void start_worker_if_needed(void) {
	if (InterlockedCompareExchange(&g_worker_running, 1, 1) == 1) {
		return;
	}
	InterlockedExchange(&g_worker_running, 1);
	g_worker_thread = CreateThread(NULL, 0, falloff_worker, NULL, 0, NULL);
	if (!g_worker_thread) {
		InterlockedExchange(&g_worker_running, 0);
		log_both("failed to start falloff worker");
	}
}

static void stop_worker(void) {
	if (InterlockedCompareExchange(&g_worker_running, 0, 1) != 1) {
		return;
	}
	if (g_worker_thread) {
		WaitForSingleObject(g_worker_thread, 6000);
		CloseHandle(g_worker_thread);
		g_worker_thread = NULL;
	}
}

MUMBLE_PLUGIN_EXPORT mumble_error_t MUMBLE_PLUGIN_CALLING_CONVENTION mumble_init(mumble_plugin_id_t id) {
	g_plugin_id = id;
	InitializeCriticalSection(&g_lock);
	g_initialized = true;
	load_config();

	WSADATA wsa_data;
	if (WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0) {
		g_ws_started = true;
	} else {
		file_log("WSAStartup failed");
	}

	log_both("loaded version=%d.%d.%d config=%s host=%s port=%d enabled=%d",
			 EXILE_VERSION_MAJOR, EXILE_VERSION_MINOR, EXILE_VERSION_PATCH,
			 g_config_path, g_config.server_host, g_config.server_port, g_config.enabled ? 1 : 0);
	return MUMBLE_STATUS_OK;
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION mumble_shutdown() {
	stop_worker();

	EnterCriticalSection(&g_lock);
	g_connection = -1;
	g_local_hash[0] = '\0';
	g_user_hash_count = 0;
	g_audio_state_count = 0;
	LeaveCriticalSection(&g_lock);

	if (g_ws_started) {
		WSACleanup();
		g_ws_started = false;
	}
	if (g_initialized) {
		DeleteCriticalSection(&g_lock);
		g_initialized = false;
	}
}

MUMBLE_PLUGIN_EXPORT struct MumbleStringWrapper MUMBLE_PLUGIN_CALLING_CONVENTION mumble_getName() {
	return wrap_static("Kongoria Voice - Spatial Audio");
}

MUMBLE_PLUGIN_EXPORT mumble_version_t MUMBLE_PLUGIN_CALLING_CONVENTION mumble_getAPIVersion() {
	mumble_version_t version = { 1, 2, 0 };
	return version;
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION mumble_registerAPIFunctions(void *apiStruct) {
	if (apiStruct) {
		g_api = MUMBLE_API_CAST(apiStruct);
		g_api_ready = true;
	}
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION mumble_releaseResource(const void *pointer) {
	(void) pointer;
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION mumble_setMumbleInfo(
	mumble_version_t mumbleVersion, mumble_version_t mumbleAPIVersion, mumble_version_t minimumExpectedAPIVersion) {
	(void) mumbleVersion;
	(void) mumbleAPIVersion;
	(void) minimumExpectedAPIVersion;
}

MUMBLE_PLUGIN_EXPORT mumble_version_t MUMBLE_PLUGIN_CALLING_CONVENTION mumble_getVersion() {
	mumble_version_t version = { EXILE_VERSION_MAJOR, EXILE_VERSION_MINOR, EXILE_VERSION_PATCH };
	return version;
}

MUMBLE_PLUGIN_EXPORT struct MumbleStringWrapper MUMBLE_PLUGIN_CALLING_CONVENTION mumble_getAuthor() {
	return wrap_static("Kongoria Voice");
}

MUMBLE_PLUGIN_EXPORT struct MumbleStringWrapper MUMBLE_PLUGIN_CALLING_CONVENTION mumble_getDescription() {
	return wrap_static("Server-authoritative spatial voice for The Isle Evrima.");
}

MUMBLE_PLUGIN_EXPORT uint32_t MUMBLE_PLUGIN_CALLING_CONVENTION mumble_getFeatures() {
	return MUMBLE_FEATURE_AUDIO;
}

MUMBLE_PLUGIN_EXPORT uint32_t MUMBLE_PLUGIN_CALLING_CONVENTION mumble_deactivateFeatures(uint32_t features) {
	(void) features;
	return MUMBLE_FEATURE_NONE;
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION mumble_onServerSynchronized(mumble_connection_t connection) {
	g_connection = connection;
	update_local_hash();
	refresh_user_hashes();
	start_worker_if_needed();
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION mumble_onServerDisconnected(mumble_connection_t connection) {
	(void) connection;
	stop_worker();
	EnterCriticalSection(&g_lock);
	g_connection = -1;
	g_local_hash[0] = '\0';
	g_user_hash_count = 0;
	g_audio_state_count = 0;
	LeaveCriticalSection(&g_lock);
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION mumble_onUserAdded(mumble_connection_t connection,
																			 mumble_userid_t userID) {
	(void) connection;
	(void) userID;
	if (!g_local_hash[0]) {
		update_local_hash();
	}
	refresh_user_hashes();
	if (g_local_hash[0]) {
		start_worker_if_needed();
	}
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION mumble_onUserRemoved(mumble_connection_t connection,
																			   mumble_userid_t userID) {
	(void) connection;
	(void) userID;
	refresh_user_hashes();
}

MUMBLE_PLUGIN_EXPORT void MUMBLE_PLUGIN_CALLING_CONVENTION mumble_onChannelEntered(mumble_connection_t connection,
																				  mumble_userid_t userID,
																				  mumble_channelid_t previousChannelID,
																				  mumble_channelid_t newChannelID) {
	(void) connection;
	(void) userID;
	(void) previousChannelID;
	(void) newChannelID;
	if (!g_local_hash[0]) {
		update_local_hash();
	}
	refresh_user_hashes();
	if (g_local_hash[0]) {
		start_worker_if_needed();
	}
}

MUMBLE_PLUGIN_EXPORT bool MUMBLE_PLUGIN_CALLING_CONVENTION mumble_onAudioInput(short *inputPCM, uint32_t sampleCount,
																			   uint16_t channelCount,
																			   uint32_t sampleRate, bool isSpeech) {
	(void) inputPCM;
	(void) sampleCount;
	(void) channelCount;
	(void) sampleRate;
	(void) isSpeech;
	return false;
}

MUMBLE_PLUGIN_EXPORT bool MUMBLE_PLUGIN_CALLING_CONVENTION
mumble_onAudioSourceFetched(float *outputPCM, uint32_t sampleCount, uint16_t channelCount, uint32_t sampleRate,
							bool isSpeech, mumble_userid_t userID) {
	(void) sampleRate;
	if (!outputPCM || !isSpeech || !g_config.enabled || channelCount == 0) {
		return false;
	}

	char hash[EXILE_MAX_HASH] = { 0 };
	float gain = 0.0f;
	float pan = 0.0f;

	EnterCriticalSection(&g_lock);
	bool has_hash = find_hash_for_user_locked(userID, hash, sizeof(hash));
	bool has_state = has_hash && find_audio_state_locked(hash, &gain, &pan);
	LeaveCriticalSection(&g_lock);

	if (!has_state) {
		gain = 0.0f;
		pan = 0.0f;
	}

	gain = clampf(gain, 0.0f, 1.0f);
	pan = clampf(pan, -1.0f, 1.0f);

	if (channelCount >= 2) {
		float left_gain = gain * sqrtf((1.0f - pan) * 0.5f);
		float right_gain = gain * sqrtf((1.0f + pan) * 0.5f);
		for (uint32_t frame = 0; frame < sampleCount; frame++) {
			uint32_t base = frame * channelCount;
			outputPCM[base] *= left_gain;
			outputPCM[base + 1] *= right_gain;
			for (uint16_t ch = 2; ch < channelCount; ch++) {
				outputPCM[base + ch] *= gain;
			}
		}
	} else {
		for (uint32_t i = 0; i < sampleCount; i++) {
			outputPCM[i] *= gain;
		}
	}

	return true;
}

MUMBLE_PLUGIN_EXPORT bool MUMBLE_PLUGIN_CALLING_CONVENTION mumble_onAudioOutputAboutToPlay(float *outputPCM,
																						  uint32_t sampleCount,
																						  uint16_t channelCount,
																						  uint32_t sampleRate) {
	(void) outputPCM;
	(void) sampleCount;
	(void) channelCount;
	(void) sampleRate;
	return false;
}

MUMBLE_PLUGIN_EXPORT bool MUMBLE_PLUGIN_CALLING_CONVENTION mumble_hasUpdate() {
	return false;
}

MUMBLE_PLUGIN_EXPORT struct MumbleStringWrapper MUMBLE_PLUGIN_CALLING_CONVENTION mumble_getUpdateDownloadURL() {
	return wrap_static("");
}
