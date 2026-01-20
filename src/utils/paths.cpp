#include "paths.h"

#ifdef _WIN32
	#include <ShlObj.h>
#endif

namespace AltMan {

	std::filesystem::path Paths::GetPlatformAppData() {
#ifdef _WIN32
		wchar_t* appDataPath = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath))) {
			std::filesystem::path result = appDataPath;
			CoTaskMemFree(appDataPath);
			return result;
		}
		if (const char* appdata = std::getenv("APPDATA")) {
			return appdata;
		}
		return std::filesystem::temp_directory_path();
#elif defined(__APPLE__)
		if (const char* home = std::getenv("HOME")) {
			return std::filesystem::path(home) / "Library" / "Application Support";
		}
		return "/tmp";
#else
		if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
			return xdg;
		}
		if (const char* home = std::getenv("HOME")) {
			return std::filesystem::path(home) / ".local" / "share";
		}
		return "/tmp";
#endif
	}

	std::filesystem::path Paths::AppData() {
		static const auto path = GetPlatformAppData() / "AltMan";
		return path;
	}

	std::filesystem::path Paths::EnsureDir(const std::filesystem::path& path) {
		std::error_code ec;
		std::filesystem::create_directories(path, ec);
		if (ec) {
			//LOG_ERROR("Failed to create directory: ", ec.message());
		}
		return path;
	}

	std::filesystem::path Paths::Storage() {
		return EnsureDir(AppData() / "storage");
	}

	std::filesystem::path Paths::Backups() {
		return EnsureDir(AppData() / "backups");
	}

	std::filesystem::path Paths::WebViewProfiles() {
		return EnsureDir(AppData() / "WebViewProfiles" / "Roblox");
	}

	std::filesystem::path Paths::Config(std::string_view filename) {
		return EnsureDir(Storage()) / filename;
	}

	std::filesystem::path Paths::BackupFile(std::string_view filename) {
		return EnsureDir(Backups()) / filename;
	}

}