#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define IDR_KONGORIA_DLL 101
#define IDR_KONGORIA_INI 102

static bool join_path(char *out, size_t out_size, const char *left, const char *right) {
	if (!out || !left || !right) {
		return false;
	}
	int written = snprintf(out, out_size, "%s\\%s", left, right);
	return written > 0 && (size_t) written < out_size;
}

static bool ensure_directory(const char *path) {
	char buffer[MAX_PATH * 2];
	snprintf(buffer, sizeof(buffer), "%s", path);

	for (char *p = buffer; *p; p++) {
		if (*p == '\\' || *p == '/') {
			char saved = *p;
			*p = '\0';
			if (buffer[0] && buffer[strlen(buffer) - 1] != ':') {
				CreateDirectoryA(buffer, NULL);
			}
			*p = saved;
		}
	}

	return CreateDirectoryA(buffer, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool write_resource(WORD resource_id, const char *target_path, char *error, size_t error_size) {
	HRSRC res = FindResourceA(NULL, MAKEINTRESOURCEA(resource_id), RT_RCDATA);
	if (!res) {
		snprintf(error, error_size, "Missing installer resource %u.", resource_id);
		return false;
	}

	HGLOBAL loaded = LoadResource(NULL, res);
	DWORD size = SizeofResource(NULL, res);
	void *data = LockResource(loaded);
	if (!loaded || !size || !data) {
		snprintf(error, error_size, "Unable to read installer resource %u.", resource_id);
		return false;
	}

	HANDLE file = CreateFileA(target_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		snprintf(error, error_size, "Unable to write %s. Close Mumble and try again.", target_path);
		return false;
	}

	DWORD written = 0;
	BOOL ok = WriteFile(file, data, size, &written, NULL);
	CloseHandle(file);

	if (!ok || written != size) {
		snprintf(error, error_size, "Failed while writing %s.", target_path);
		return false;
	}

	return true;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show_command) {
	(void) instance;
	(void) previous;
	(void) command_line;
	(void) show_command;

	char app_data[MAX_PATH];
	if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, app_data) != S_OK) {
		MessageBoxA(NULL, "Could not find your AppData folder.", "Kongoria Voice Setup", MB_ICONERROR | MB_OK);
		return 1;
	}

	char mumble_dir[MAX_PATH * 2];
	char plugin_dir[MAX_PATH * 2];
	char dll_path[MAX_PATH * 2];
	char ini_path[MAX_PATH * 2];
	char error[1024];

	if (!join_path(mumble_dir, sizeof(mumble_dir), app_data, "Mumble\\Mumble")) {
		MessageBoxA(NULL, "Could not build the Mumble folder path.", "Kongoria Voice Setup", MB_ICONERROR | MB_OK);
		return 1;
	}
	if (!join_path(plugin_dir, sizeof(plugin_dir), mumble_dir, "Plugins")) {
		MessageBoxA(NULL, "Could not build the plugin folder path.", "Kongoria Voice Setup", MB_ICONERROR | MB_OK);
		return 1;
	}
	if (!ensure_directory(plugin_dir)) {
		MessageBoxA(NULL, "Could not create the Mumble plugin folder.", "Kongoria Voice Setup", MB_ICONERROR | MB_OK);
		return 1;
	}

	join_path(dll_path, sizeof(dll_path), plugin_dir, "kongoria_voice.dll");
	join_path(ini_path, sizeof(ini_path), plugin_dir, "kongoria_voice.ini");

	if (!write_resource(IDR_KONGORIA_DLL, dll_path, error, sizeof(error))) {
		MessageBoxA(NULL, error, "Kongoria Voice Setup", MB_ICONERROR | MB_OK);
		return 1;
	}

	bool ini_existed = GetFileAttributesA(ini_path) != INVALID_FILE_ATTRIBUTES;
	if (!ini_existed) {
		if (!write_resource(IDR_KONGORIA_INI, ini_path, error, sizeof(error))) {
			MessageBoxA(NULL, error, "Kongoria Voice Setup", MB_ICONERROR | MB_OK);
			return 1;
		}
	}

	char message[1200];
	snprintf(message, sizeof(message),
			 "Kongoria Voice plugin installed.\n\nPlugin:\n%s\n\nConfig:\n%s\n%s\n\nRestart Mumble, then enable/check \"Kongoria Voice - Spatial Audio\".",
			 dll_path, ini_path,
			 ini_existed ? "\nExisting config was kept." : "\nDefault config was created. Edit it if your server host or port changed.");
	MessageBoxA(NULL, message, "Kongoria Voice Setup", MB_ICONINFORMATION | MB_OK);
	return 0;
}
