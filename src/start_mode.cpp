#include "start_mode.h"
#include "LowTasks.h"
#include "global.h"
#include "logging.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace start_mode
{

StartMode mode = mode_none;
std::int32_t restart_from = -1;

std::string &rtrim(std::string &str, const std::string &chars = "\t\n\v\f\r ")
{
    str.erase(str.find_last_not_of(chars) + 1);
    return str;
}

void configure_start_mode()
{
    if (!CPU_Master) {
	return;
    }

    switch (mode) {
    case mode_start:
	if (!std::filesystem::is_empty(OUTPUTDIR)) {
	    std::string backup_path = OUTPUTDIR;
	    rtrim(backup_path, "/");
	    backup_path += "_bak";

	    for (uint32_t i = 1; std::filesystem::exists(backup_path); i++) {
		backup_path = OUTPUTDIR;
		rtrim(backup_path, "/");
		backup_path += "_bak" + std::to_string(i);
	    }
	    logging::print_master(LOG_INFO
				  "%s is not empty, backing up as %s\n",
				  OUTPUTDIR, backup_path.c_str());
	    std::filesystem::rename(OUTPUTDIR, backup_path);
	    std::filesystem::create_directory(OUTPUTDIR);
	}
	break;
    case mode_auto:
	if (std::filesystem::is_empty(OUTPUTDIR)) {
	    logging::print_master(
		LOG_INFO "No output found, starting fresh simulation\n");
	    mode = mode_start;
	    break;
	} else {
	    mode = mode_restart;
	    // continue with case mode_restart to get the last outputfile
	}
    case mode_restart:
	if (restart_from < 0) {
	    restart_from = get_latest_output_num();
	}

	if (restart_from < 0) {
	    die("Can't restart, no valid output file found. Check misc.dat");
	}
	break;
    default:
	die("Invalid start_mode");
    }
}

std::string get_last_line(std::ifstream &in)
{
    std::string line;
    while (in >> std::ws && std::getline(in, line));

    return line;
}

bool is_number(std::string s)
{
    for (std::uint32_t i = 0; i < s.length(); i++)
	if (isdigit(s[i]) == false)
	    return false;

    return true;
}

std::int32_t get_latest_output_num()
{
    std::ifstream misc_file;
    std::filesystem::path path;

    path = OUTPUTDIR;
    path /= "misc.dat";

    misc_file.open(path);

    if (!misc_file.is_open()) {
	return -1;
    }

    // find last line
    std::string last_line = get_last_line(misc_file);
    misc_file.close();

    std::string first_word =
	last_line.substr(0, last_line.find_first_of(" \t"));

    if (first_word.length() < 1 || !is_number(first_word)) {
	return -1;
    } else {
	return std::stoi(first_word);
    }
}

} // namespace start_mode
