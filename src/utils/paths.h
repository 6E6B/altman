#pragma once
#include <filesystem>

namespace AltMan {

	class Paths {
		public:
			static std::filesystem::path AppData();

			static std::filesystem::path Storage();
			static std::filesystem::path Backups();
			static std::filesystem::path WebViewProfiles();
			static std::filesystem::path Config(std::string_view filename);

			static std::filesystem::path EnsureDir(const std::filesystem::path& path);

		private:
			static std::filesystem::path GetPlatformAppData();
	};

}