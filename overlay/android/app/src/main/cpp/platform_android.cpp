#include "platform_android.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <atomic>
#include <string>
#include <vector>

namespace platform {

static char gStorageRoot[512] = "";
static char gGameDataRoot[512] = "";

static void *
stdioPump(void *)
{
	static char line[1024];
	size_t used = 0;
	ssize_t got;
	char c;
	while((got = read(STDIN_FILENO, &c, 1)) == 1){
		if(c == '\n' || used == sizeof(line) - 1){
			line[used] = '\0';
			if(used > 0)
				__android_log_write(ANDROID_LOG_INFO, "MiamiVR-game", line);
			used = 0;
		}else if(c != '\r'){
			line[used++] = c;
		}
	}
	return nullptr;
}

static std::atomic<const char *> gCheckpoint{"start"};
static std::atomic<unsigned long> gCheckpointSeq{0};

void
setCheckpoint(const char *label)
{
	gCheckpoint.store(label, std::memory_order_relaxed);
	gCheckpointSeq.fetch_add(1, std::memory_order_relaxed);
}

static void *
stallWatchdog(void *)
{
	unsigned long lastSeq = 0;
	int stalledFor = 0;
	for(;;){
		sleep(1);
		const unsigned long seq = gCheckpointSeq.load(std::memory_order_relaxed);
		if(seq != lastSeq){
			lastSeq = seq;
			stalledFor = 0;
			continue;
		}
		stalledFor++;
		if(stalledFor == 3 || stalledFor % 10 == 0)
			LOGE("stalled %ds at checkpoint '%s'", stalledFor,
			     gCheckpoint.load(std::memory_order_relaxed));
	}
	return nullptr;
}

void
startStallWatchdog(void)
{
	pthread_t thread;
	if(pthread_create(&thread, nullptr, stallWatchdog, nullptr) == 0)
		pthread_detach(thread);
}

void
redirectStdioToLog(void)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);

	int fds[2];
	if(pipe(fds) != 0){
		LOGE("cannot create the stdio pipe");
		return;
	}
	// Read end becomes stdin for the pump thread, so it can use the plain
	// read() above without carrying the descriptor around.
	dup2(fds[0], STDIN_FILENO);
	dup2(fds[1], STDOUT_FILENO);
	dup2(fds[1], STDERR_FILENO);

	pthread_t thread;
	if(pthread_create(&thread, nullptr, stdioPump, nullptr) != 0){
		LOGE("cannot start the stdio pump");
		return;
	}
	pthread_detach(thread);
	LOGI("stdout/stderr now go to logcat as MiamiVR-game");
}

void
setStorageRoot(const char *path)
{
	if(path == nullptr || path[0] == '\0')
		return;
	snprintf(gStorageRoot, sizeof(gStorageRoot), "%s", path);
	snprintf(gGameDataRoot, sizeof(gGameDataRoot), "%s/gamedata", path);
	mkdir(gGameDataRoot, 0770);
	LOGI("storage root: %s", gStorageRoot);
	LOGI("game data root: %s", gGameDataRoot);
}

const char *storageRoot(void) { return gStorageRoot; }
const char *gameDataRoot(void) { return gGameDataRoot; }

char *
resolveGamePath(char *buffer, size_t bufferSize, const char *relative)
{
	while(*relative == '\\' || *relative == '/')
		relative++;
	snprintf(buffer, bufferSize, "%s/%s", gGameDataRoot, relative);
	for(char *p = buffer; *p; p++)
		if(*p == '\\')
			*p = '/';
	return buffer;
}

// Walks the requested path one component at a time, matching each against the
// real directory entries without regard to case. Only the components that do
// not already exist verbatim cost a directory scan.
bool
resolveGamePathCaseInsensitive(char *buffer, size_t bufferSize, const char *relative)
{
	char normalised[512];
	resolveGamePath(normalised, sizeof(normalised), relative);

	struct stat st;
	if(stat(normalised, &st) == 0){
		snprintf(buffer, bufferSize, "%s", normalised);
		return true;
	}

	std::string resolved = gGameDataRoot;
	const char *cursor = normalised + strlen(gGameDataRoot);
	while(*cursor == '/')
		cursor++;

	std::string remainder = cursor;
	size_t start = 0;
	while(start <= remainder.size()){
		size_t slash = remainder.find('/', start);
		std::string component = remainder.substr(
			start, slash == std::string::npos ? std::string::npos : slash - start);
		if(component.empty() && slash == std::string::npos)
			break;

		std::string candidate = resolved + "/" + component;
		if(stat(candidate.c_str(), &st) != 0){
			DIR *dir = opendir(resolved.c_str());
			if(dir == nullptr)
				return false;
			bool found = false;
			struct dirent *entry;
			while((entry = readdir(dir)) != nullptr){
				if(strcasecmp(entry->d_name, component.c_str()) == 0){
					candidate = resolved + "/" + entry->d_name;
					found = true;
					break;
				}
			}
			closedir(dir);
			if(!found)
				return false;
		}
		resolved = candidate;

		if(slash == std::string::npos)
			break;
		start = slash + 1;
	}

	snprintf(buffer, bufferSize, "%s", resolved.c_str());
	return true;
}

} // namespace platform

// ---------------------------------------------------------------------------
// INI implementation
// ---------------------------------------------------------------------------

namespace {

char *
trim(char *s)
{
	while(*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;
	char *end = s + strlen(s);
	while(end > s && (end[-1] == ' ' || end[-1] == '\t' ||
	                  end[-1] == '\r' || end[-1] == '\n'))
		end--;
	*end = '\0';
	return s;
}

bool
readLines(const char *fileName, std::vector<std::string> &lines)
{
	FILE *f = fopen(fileName, "rb");
	if(f == nullptr)
		return false;
	std::string current;
	int c;
	while((c = fgetc(f)) != EOF){
		if(c == '\n'){
			lines.push_back(current);
			current.clear();
		}else if(c != '\r'){
			current.push_back((char)c);
		}
	}
	if(!current.empty())
		lines.push_back(current);
	fclose(f);
	return true;
}

// Returns true when the line is "[wanted]".
bool
isSectionHeader(const std::string &line, std::string *nameOut)
{
	std::string copy = line;
	char *t = trim(&copy[0]);
	size_t len = strlen(t);
	if(len < 2 || t[0] != '[' || t[len-1] != ']')
		return false;
	if(nameOut != nullptr)
		nameOut->assign(t + 1, len - 2);
	return true;
}

// Splits "key = value"; returns false for blanks and comments.
bool
splitEntry(const std::string &line, std::string *keyOut, std::string *valueOut)
{
	std::string copy = line;
	char *t = trim(&copy[0]);
	if(t[0] == '\0' || t[0] == ';' || t[0] == '#')
		return false;
	char *eq = strchr(t, '=');
	if(eq == nullptr)
		return false;
	*eq = '\0';
	if(keyOut != nullptr)
		keyOut->assign(trim(t));
	if(valueOut != nullptr)
		valueOut->assign(trim(eq + 1));
	return true;
}

bool
lookup(const char *section, const char *key, const char *fileName,
       std::string &valueOut)
{
	std::vector<std::string> lines;
	if(!readLines(fileName, lines))
		return false;

	bool inSection = false;
	std::string name, entryKey, entryValue;
	for(const std::string &line : lines){
		if(isSectionHeader(line, &name)){
			inSection = strcasecmp(name.c_str(), section) == 0;
			continue;
		}
		if(!inSection)
			continue;
		if(!splitEntry(line, &entryKey, &entryValue))
			continue;
		if(strcasecmp(entryKey.c_str(), key) == 0){
			valueOut = entryValue;
			return true;
		}
	}
	return false;
}

} // namespace

extern "C" int32_t
GetPrivateProfileIntA(const char *section, const char *key,
                      int32_t defaultValue, const char *fileName)
{
	std::string value;
	if(!lookup(section, key, fileName, value) || value.empty())
		return defaultValue;
	// Win32 stops at the first non-numeric character and treats a leading
	// minus as valid; strtol matches that closely enough for the settings and
	// calibration values the VR layer stores.
	return (int32_t)strtol(value.c_str(), nullptr, 10);
}

extern "C" uint32_t
GetPrivateProfileStringA(const char *section, const char *key,
                         const char *defaultValue, char *out,
                         uint32_t outSize, const char *fileName)
{
	if(out == nullptr || outSize == 0)
		return 0;

	std::string value;
	if(!lookup(section, key, fileName, value))
		value = defaultValue != nullptr ? defaultValue : "";

	uint32_t copied = (uint32_t)value.size();
	if(copied > outSize - 1)
		copied = outSize - 1;
	memcpy(out, value.c_str(), copied);
	out[copied] = '\0';
	return copied;
}

extern "C" int32_t
WritePrivateProfileStringA(const char *section, const char *key,
                           const char *value, const char *fileName)
{
	std::vector<std::string> lines;
	readLines(fileName, lines);	// absent file is fine, we create it

	std::string name, entryKey;
	int sectionStart = -1;
	int sectionEnd = (int)lines.size();

	for(size_t i = 0; i < lines.size(); i++){
		if(!isSectionHeader(lines[i], &name))
			continue;
		if(sectionStart >= 0){
			sectionEnd = (int)i;
			break;
		}
		if(strcasecmp(name.c_str(), section) == 0)
			sectionStart = (int)i;
	}

	std::string entry = std::string(key) + " = " + (value ? value : "");

	if(sectionStart < 0){
		if(!lines.empty() && !lines.back().empty())
			lines.push_back("");
		lines.push_back(std::string("[") + section + "]");
		lines.push_back(entry);
	}else{
		bool replaced = false;
		for(int i = sectionStart + 1; i < sectionEnd; i++){
			if(!splitEntry(lines[i], &entryKey, nullptr))
				continue;
			if(strcasecmp(entryKey.c_str(), key) == 0){
				lines[i] = entry;
				replaced = true;
				break;
			}
		}
		if(!replaced){
			// Append at the end of the section, before any trailing blanks,
			// so repeated writes do not accumulate empty lines.
			int insertAt = sectionEnd;
			while(insertAt > sectionStart + 1 && lines[insertAt-1].empty())
				insertAt--;
			lines.insert(lines.begin() + insertAt, entry);
		}
	}

	FILE *f = fopen(fileName, "wb");
	if(f == nullptr){
		LOGE("WritePrivateProfileStringA: cannot open %s", fileName);
		return 0;
	}
	for(const std::string &line : lines)
		fprintf(f, "%s\n", line.c_str());
	fclose(f);
	return 1;
}
