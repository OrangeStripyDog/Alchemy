/** 
 * @file llerror.cpp
 * @date   December 2006
 * @brief error message system
 *
 * $LicenseInfo:firstyear=2006&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * 
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "llerror.h"
#include "llerrorcontrol.h"
#include "llsdutil.h"

#include <cctype>
#ifdef __GNUC__
# include <cxxabi.h>
#endif // __GNUC__
#include <sstream>
#if !LL_WINDOWS
# include <syslog.h>
# include <unistd.h>
# include <sys/stat.h>
#else
# include <io.h>
#endif // !LL_WINDOWS
#include <vector>
#include <string_view>
#include "string.h"

#include "absl/synchronization/mutex.h"
#include "absl/container/flat_hash_map.h"

#include "llapp.h"
#include "llapr.h"
#include "llfile.h"
#include "lllivefile.h"
#include "llsd.h"
#include "llsdserialize.h"
#include "llsingleton.h"
#include "llstl.h"
#include "lltimer.h"

#include <boost/make_shared.hpp>

// On Mac, got:
// #error "Boost.Stacktrace requires `_Unwind_Backtrace` function. Define
// `_GNU_SOURCE` macro or `BOOST_STACKTRACE_GNU_SOURCE_NOT_REQUIRED` if
// _Unwind_Backtrace is available without `_GNU_SOURCE`."
#define BOOST_STACKTRACE_GNU_SOURCE_NOT_REQUIRED
#if LL_WINDOWS
// On Windows, header-only implementation causes macro collisions -- use
// prebuilt library
#define BOOST_STACKTRACE_LINK
#endif // LL_WINDOWS
#include <boost/stacktrace.hpp>

namespace {
#if LL_WINDOWS
	void debugger_print(const std::string& s)
	{
		// Be careful when calling OutputDebugString as it throws DBG_PRINTEXCEPTION_C 
		// which works just fine under the windows debugger, but can cause users who
		// have enabled SEHOP exception chain validation to crash due to interactions
		// between the Win 32-bit exception handling and boost coroutine fiber stacks. BUG-2707
		//
		if (IsDebuggerPresent())
		{
			// Need UTF16 for Unicode OutputDebugString
			//
			if (s.size())
			{
				OutputDebugString(ll_convert_string_to_wide(s).c_str());
				OutputDebugString(TEXT("\n"));
			}
		}
	}
#else
	class RecordToSyslog : public LLError::Recorder
	{
	public:
		RecordToSyslog(const std::string& identity)
			: mIdentity(identity)
		{
			openlog(mIdentity.c_str(), LOG_CONS|LOG_PID, LOG_LOCAL0);
				// we need to set the string from a local copy of the string
				// since apparanetly openlog expects the const char* to remain
				// valid even after it returns (presumably until closelog)
		}
		
		~RecordToSyslog()
		{
			closelog();
		}

        virtual bool enabled() override
        {
            return LLError::getEnabledLogTypesMask() & 0x01;
        }
        
		virtual void recordMessage(LLError::ELevel level,
									const std::string& message) override
		{
			int syslogPriority = LOG_CRIT;
			switch (level) {
				case LLError::LEVEL_DEBUG:	syslogPriority = LOG_DEBUG;	break;
				case LLError::LEVEL_INFO:	syslogPriority = LOG_INFO;	break;
				case LLError::LEVEL_WARN:	syslogPriority = LOG_WARNING; break;
				case LLError::LEVEL_ERROR:	syslogPriority = LOG_CRIT;	break;
				default:					syslogPriority = LOG_CRIT;
			}
			
			syslog(syslogPriority, "%s", message.c_str());
		}
	private:
		std::string mIdentity;
	};
#endif

	class RecordToFile : public LLError::Recorder
	{
	public:
		RecordToFile(const std::string& filename):
			mName(filename)
		{
			showMultiline(true);

			mFile.open(filename, std::ios_base::out | std::ios_base::app);
			if (!mFile)
			{
				LL_INFOS() << "Error setting log file to " << filename << LL_ENDL;
			}
			else
			{
				if (!LLError::getAlwaysFlush())
				{
					mFile.sync_with_stdio(false);
				}
			}
		}

		~RecordToFile()
		{
			mFile.close();
		}

        virtual bool enabled() override
        {
#ifdef LL_RELEASE_FOR_DOWNLOAD
            return 1;
#else
            return LLError::getEnabledLogTypesMask() & 0x02;
#endif
        }
        
        bool okay() const { return mFile.good(); }

        std::string getFilename() const { return mName; }

        virtual void recordMessage(LLError::ELevel level,
                                    const std::string& message) override
        {
            if (LLError::getAlwaysFlush())
            {
                mFile << message << std::endl;
            }
            else
            {
                mFile << message << "\n";
            }
        }

	private:
		const std::string mName;
		llofstream mFile;
	};
	
	
	class RecordToStderr : public LLError::Recorder
	{
	public:
		RecordToStderr(bool timestamp) : mUseANSI(checkANSI()) 
		{
            this->showMultiline(true);
		}
		
        virtual bool enabled() override
        {
            return LLError::getEnabledLogTypesMask() & 0x04;
        }
        
        LL_FORCE_INLINE std::string createANSI(const std::string& color)
        {
            std::string ansi_code;
            ansi_code  += '\033';
            ansi_code  += "[";
            ansi_code  += color;
            ansi_code += "m";
            return ansi_code;
        }

		virtual void recordMessage(LLError::ELevel level,
					   const std::string& message) override
		{
            static std::string s_ansi_error = createANSI("31"); // red
            static std::string s_ansi_warn  = createANSI("34"); // blue
            static std::string s_ansi_debug = createANSI("35"); // magenta

			if (mUseANSI)
			{
                writeANSI((level == LLError::LEVEL_ERROR) ? s_ansi_error :
                          (level == LLError::LEVEL_WARN)  ? s_ansi_warn :
                                                            s_ansi_debug, message);
			}
            else
            {
                 fprintf(stderr, "%s\n", message.c_str());
            }
		}
	
	private:
		bool mUseANSI;

        LL_FORCE_INLINE void writeANSI(const std::string& ansi_code, const std::string& message)
		{
            static std::string s_ansi_bold  = createANSI("1");  // bold
            static std::string s_ansi_reset = createANSI("0");  // reset
			// ANSI color code escape sequence, message, and reset in one fprintf call
            // Default all message levels to bold so we can distinguish our own messages from those dumped by subprocesses and libraries.
			fprintf(stderr, "%s%s%s\n%s", s_ansi_bold.c_str(), ansi_code.c_str(), message.c_str(), s_ansi_reset.c_str() );
		}

		static bool checkANSI(void)
		{
			// Check whether it's okay to use ANSI; if stderr is
			// a tty then we assume yes.  Can be turned off with
			// the LL_NO_ANSI_COLOR env var.
			return (0 != isatty(2)) &&
				(NULL == getenv("LL_NO_ANSI_COLOR"));
		}
	};

	class RecordToFixedBuffer : public LLError::Recorder
	{
	public:
		RecordToFixedBuffer(LLLineBuffer* buffer)
            : mBuffer(buffer)
            {
                this->showMultiline(true);
                this->showTags(false);
                this->showLocation(false);
            }
		
        virtual bool enabled() override
        {
            return LLError::getEnabledLogTypesMask() & 0x08;
        }
        
		virtual void recordMessage(LLError::ELevel level,
								   const std::string& message) override
		{
			mBuffer->addLine(message);
		}
	
	private:
		LLLineBuffer* mBuffer;
	};

#if LL_WINDOWS
	class RecordToWinDebug: public LLError::Recorder
	{
	public:
		RecordToWinDebug()
		{
            this->showMultiline(true);
            this->showTags(false);
            this->showLocation(false);
        }

        virtual bool enabled() override
        {
            return LLError::getEnabledLogTypesMask() & 0x10;
        }
        
		virtual void recordMessage(LLError::ELevel level,
								   const std::string& message) override
		{
			debugger_print(message);
		}
	};
#endif
}


namespace
{
	std::string className(const std::type_info& type)
	{
		return LLError::Log::demangle(type.name());
	}
} // anonymous

namespace LLError
{
#if LL_WINDOWS
	std::string Log::demangle(const std::string_view mangled)
#else
	std::string Log::demangle(const char* mangled)
#endif
	{
#ifdef __GNUC__
		// GCC: type_info::name() returns a mangled class name,st demangle
		// passing nullptr, 0 forces allocation of a unique buffer we can free
		// fixing MAINT-8724 on OSX 10.14
		int status = -1;
		char* name = abi::__cxa_demangle(mangled, nullptr, 0, &status);
		std::string result(name ? name : mangled);
		free(name);
		return result;

#elif LL_WINDOWS
		using namespace std::literals;
		// Visual Studio: type_info::name() includes the text "class " at the start
		static constexpr auto class_prefix = "class "sv;
		static constexpr auto struct_prefix = "struct "sv;
		if (0 == mangled.compare(0, class_prefix.length(), class_prefix))
		{
			return std::string(mangled.substr(class_prefix.length()));
		}
		else if (0 == mangled.compare(0, struct_prefix.length(), struct_prefix))
		{
			return std::string(mangled.substr(struct_prefix.length()));
		}

		// huh, that's odd, we should see one or the other prefix -- but don't
		// try to log unless logging is already initialized
		// in Python, " or ".join(vector) -- but in C++, a PITB
		LL_DEBUGS() << "Did not see 'class' or 'struct' prefix on '"
			<< mangled << "'" << LL_ENDL;
		return std::string(mangled);

#else  // neither GCC nor Visual Studio
		return mangled;
#endif
	}
} // LLError

namespace
{
	std::string functionName(const std::string& preprocessor_name)
	{
#if LL_WINDOWS
		// DevStudio: the __FUNCTION__ macro string includes
		// the type and/or namespace prefixes

		std::string::size_type p = preprocessor_name.rfind(':');
		if (p == std::string::npos)
		{
			return preprocessor_name;
		}
		return preprocessor_name.substr(p + 1);

#else
		return preprocessor_name;
#endif
	}


	class LogControlFile : public LLLiveFile
	{
		LOG_CLASS(LogControlFile);
	
	public:
		static LogControlFile& fromDirectory(const std::string& user_dir, const std::string& app_dir);
		
		virtual bool loadFile();
		
	private:
		LogControlFile(const std::string &filename)
			: LLLiveFile(filename)
			{ }
	};

	LogControlFile& LogControlFile::fromDirectory(const std::string& user_dir, const std::string& app_dir)
	{
        // NB: We have no abstraction in llcommon  for the "proper"
        // delimiter but it turns out that "/" works on all three platforms
			
		std::string file = user_dir + "/logcontrol-dev.xml";
		
		llstat stat_info;
		if (LLFile::stat(file, &stat_info)) {
			// NB: stat returns non-zero if it can't read the file, for example
			// if it doesn't exist.  LLFile has no better abstraction for 
			// testing for file existence.
			
			file = app_dir + "/logcontrol.xml";
		}
		return * new LogControlFile(file);
			// NB: This instance is never freed
	}
	
	bool LogControlFile::loadFile()
	{
		LLSD configuration;

		{
			llifstream file(filename().c_str());
			if (!file.is_open())
			{
				LL_WARNS() << filename() << " failed to open file; not changing configuration" << LL_ENDL;
				return false;
			}

			if (LLSDSerialize::fromXML(configuration, file) == LLSDParser::PARSE_FAILURE)
			{
				LL_WARNS() << filename() << " parcing error; not changing configuration" << LL_ENDL;
				return false;
			}

			if (! configuration || !configuration.isMap())
			{
				LL_WARNS() << filename() << " missing, ill-formed, or simply undefined"
							" content; not changing configuration"
						<< LL_ENDL;
				return false;
			}
		}
		
		LLError::configure(configuration);
		LL_INFOS("LogControlFile") << "logging reconfigured from " << filename() << LL_ENDL;
		return true;
	}


	typedef absl::flat_hash_map<std::string, LLError::ELevel> LevelMap;
	typedef std::vector<LLError::RecorderPtr> Recorders;
	typedef std::vector<LLError::CallSite*> CallSiteVector;

	class Globals
	{
    public:
        static Globals* getInstance();
    protected:
		Globals();
	public:
		std::ostringstream messageStream;
		bool messageStreamInUse;
		std::string mFatalMessage;

		void addCallSite(LLError::CallSite&);
		void invalidateCallSites();

	private:
		CallSiteVector callSites;
	};

	Globals::Globals()
		: messageStream(),
		messageStreamInUse(false),
		callSites()
	{
	}

    Globals* Globals::getInstance()
    {
        // According to C++11 Function-Local Initialization
        // of static variables is supposed to be thread safe
        // without risk of deadlocks.
        static Globals inst;

        return &inst;
    }

	void Globals::addCallSite(LLError::CallSite& site)
	{
		callSites.push_back(&site);
	}
	
	void Globals::invalidateCallSites()
	{
		for (CallSiteVector::const_iterator i = callSites.begin();
			 i != callSites.end();
			 ++i)
		{
            (*i)->invalidate();
		}
		
		callSites.clear();
	}
}

namespace LLError
{
	class SettingsConfig : public LLRefCount
	{
		friend class Settings;

	public:
		virtual ~SettingsConfig();

		LLError::ELevel                     mDefaultLevel;

        bool 								mLogAlwaysFlush;

        U32 								mEnabledLogTypesMask;

		LevelMap                            mFunctionLevelMap;
		LevelMap                            mClassLevelMap;
		LevelMap                            mFileLevelMap;
		LevelMap                            mTagLevelMap;
		absl::flat_hash_map<std::string, unsigned int> mUniqueLogMessages;
		
		LLError::FatalFunction              mCrashFunction;
		LLError::TimeFunction               mTimeFunction;

		Recorders                           mRecorders;

		int                                 mShouldLogCallCounter;

	private:
		SettingsConfig();
	};

	typedef LLPointer<SettingsConfig> SettingsConfigPtr;

	class Settings
	{
    public:
        static Settings* getInstance();
    protected:
		Settings();
	public:
		SettingsConfigPtr getSettingsConfig();

		void reset();
		SettingsStoragePtr saveAndReset(); 
		void restore(SettingsStoragePtr pSettingsStorage);
		
	private:
		SettingsConfigPtr mSettingsConfig;
	};

	SettingsConfig::SettingsConfig()
		: LLRefCount(),
		mDefaultLevel(LLError::LEVEL_DEBUG),
		mLogAlwaysFlush(true),
		mEnabledLogTypesMask(255),
		mFunctionLevelMap(),
		mClassLevelMap(),
		mFileLevelMap(),
		mTagLevelMap(),
		mUniqueLogMessages(),
		mCrashFunction(NULL),
		mTimeFunction(NULL),
		mRecorders(),
		mShouldLogCallCounter(0)
	{
	}

	SettingsConfig::~SettingsConfig()
	{
		mRecorders.clear();
	}

	Settings::Settings():
		mSettingsConfig(new SettingsConfig())
	{
	}

    Settings* Settings::getInstance()
    {
        // According to C++11 Function-Local Initialization
        // of static variables is supposed to be thread safe
        // without risk of deadlocks.
        static Settings inst;

        return &inst;
    }

	SettingsConfigPtr Settings::getSettingsConfig()
	{
		return mSettingsConfig;
	}

	void Settings::reset()
	{
		Globals::getInstance()->invalidateCallSites();
		mSettingsConfig = new SettingsConfig();
	}

	SettingsStoragePtr Settings::saveAndReset()
	{
		SettingsStoragePtr oldSettingsConfig(mSettingsConfig.get());
		reset();
		return oldSettingsConfig;
	}

	void Settings::restore(SettingsStoragePtr pSettingsStorage)
	{
		Globals::getInstance()->invalidateCallSites();
		SettingsConfigPtr newSettingsConfig(dynamic_cast<SettingsConfig *>(pSettingsStorage.get()));
		mSettingsConfig = std::move(newSettingsConfig);
	}
}

namespace LLError
{
	CallSite::CallSite(ELevel level,
					const char* file,
					int line,
					const std::type_info& class_info, 
					const char* function, 
					bool printOnce,
					const char** tags, 
					size_t tag_count)
	:	mLevel(level), 
		mFile(file), 
		mLine(line),
		mClassInfo(class_info), 
		mFunction(function),
		mCached(false), 
		mShouldLog(false), 
		mPrintOnce(printOnce),
		mTags(new const char* [tag_count]),
		mTagCount(tag_count)
	{
		switch (mLevel)
		{
        case LEVEL_DEBUG: mLevelString = "DEBUG";   break;
        case LEVEL_INFO:  mLevelString = "INFO";    break;
        case LEVEL_WARN:  mLevelString = "WARNING"; break;
        case LEVEL_ERROR: mLevelString = "ERROR";   break;
        default:          mLevelString = "XXX";     break;
		};

		mLocationString = llformat("%s(%d)", abbreviateFile(mFile).c_str(), mLine);
#if LL_WINDOWS
		// DevStudio: __FUNCTION__ already includes the full class name
#else
#if LL_LINUX
		// gross, but typeid comparison seems to always fail here with gcc4.1
		if (0 != strcmp(mClassInfo.name(), typeid(NoClassInfo).name()))
#else
		if (mClassInfo != typeid(NoClassInfo))
#endif // LL_LINUX
		{
			mFunctionString = className(mClassInfo) + "::";
		}
#endif
		mFunctionString += std::string(mFunction);

		for (int i = 0; i < tag_count; i++)
		{
            if (strchr(tags[i], ' '))
            {
                LL_ERRS() << "Space is not allowed in a log tag at " << mLocationString << LL_ENDL;
            }
			mTags[i] = tags[i];
		}

        mTagString.append("#");
        // always construct a tag sequence; will be just a single # if no tag
		for (size_t i = 0; i < mTagCount; i++)
		{
			mTagString.append(mTags[i]);
            mTagString.append("#");
		}
	}

	CallSite::~CallSite()
	{
		delete []mTags;
	}

	void CallSite::invalidate()
	{
		mCached = false; 
	}
}

namespace
{
    bool shouldLogToStderr()
    {
#if LL_DARWIN
        // On Mac OS X, stderr from apps launched from the Finder goes to the
        // console log.  It's generally considered bad form to spam too much
        // there. That scenario can be detected by noticing that stderr is a
        // character device (S_IFCHR).

        // If stderr is a tty or a pipe, assume the user launched from the
        // command line or debugger and therefore wants to see stderr.
        if (isatty(STDERR_FILENO))
            return true;
        // not a tty, but might still be a pipe -- check
        struct stat st;
        if (fstat(STDERR_FILENO, &st) < 0)
        {
            // capture errno right away, before engaging any other operations
            auto errno_save = errno;
            // this gets called during log-system setup -- can't log yet!
            std::cerr << "shouldLogToStderr: fstat(" << STDERR_FILENO << ") failed, errno "
                      << errno_save << std::endl;
            // if we can't tell, err on the safe side and don't write stderr
            return false;
        }

        // fstat() worked: return true only if stderr is a pipe
        return ((st.st_mode & S_IFMT) == S_IFIFO);
#else
        return true;
#endif
    }

	bool stderrLogWantsTime()
	{
#if LL_WINDOWS
		return false;
#else
		return true;
#endif
	}
	
	
	void commonInit(const std::string& user_dir, const std::string& app_dir, bool log_to_stderr = true)
	{
		LLError::Settings::getInstance()->reset();

		LLError::setDefaultLevel(LLError::LEVEL_INFO);
		LLError::setAlwaysFlush(true);
		LLError::setEnabledLogTypesMask(0xFFFFFFFF);
		LLError::setFatalFunction(LLError::crashAndLoop);
		LLError::setTimeFunction(LLError::utcTime);

		// log_to_stderr is only false in the unit and integration tests to keep builds quieter
		if (log_to_stderr && shouldLogToStderr())
		{
			LLError::logToStderr();
		}

#if LL_WINDOWS
		LLError::RecorderPtr recordToWinDebug = boost::make_shared<RecordToWinDebug>();
		LLError::addRecorder(std::move(recordToWinDebug));
#endif

		LogControlFile& e = LogControlFile::fromDirectory(user_dir, app_dir);

		// NOTE: We want to explicitly load the file before we add it to the event timer
		// that checks for changes to the file.  Else, we're not actually loading the file yet,
		// and most of the initialization happens without any attention being paid to the
		// log control file.  Not to mention that when it finally gets checked later,
		// all log statements that have been evaluated already become dirty and need to be
		// evaluated for printing again.  So, make sure to call checkAndReload()
		// before addToEventTimer().
		e.checkAndReload();
		e.addToEventTimer();
	}
}

namespace LLError
{
	void initForApplication(const std::string& user_dir, const std::string& app_dir, bool log_to_stderr)
	{
		commonInit(user_dir, app_dir, log_to_stderr);
	}

	void setFatalFunction(const FatalFunction& f)
	{
		SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
		s->mCrashFunction = f;
	}

	FatalFunction getFatalFunction()
	{
		SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
		return s->mCrashFunction;
	}

	std::string getFatalMessage()
	{
		return Globals::getInstance()->mFatalMessage;
	}

	void setTimeFunction(TimeFunction f)
	{
		SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
		s->mTimeFunction = f;
	}

	void setDefaultLevel(ELevel level)
	{
		Globals::getInstance()->invalidateCallSites();
		SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
		s->mDefaultLevel = level;
	}

	ELevel getDefaultLevel()
	{
		SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
		return s->mDefaultLevel;
	}

	void setAlwaysFlush(bool flush)
	{
		SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
		s->mLogAlwaysFlush = flush;
	}

	bool getAlwaysFlush()
	{
		SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
		return s->mLogAlwaysFlush;
	}

	void setEnabledLogTypesMask(U32 mask)
	{
		SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
		s->mEnabledLogTypesMask = mask;
	}

	U32 getEnabledLogTypesMask()
	{
		SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
		return s->mEnabledLogTypesMask;
	}

	void setFunctionLevel(const std::string& function_name, ELevel level)
	{
		Globals::getInstance()->invalidateCallSites();
		SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
		s->mFunctionLevelMap[function_name] = level;
	}

	void setClassLevel(const std::string& class_name, ELevel level)
	{
		Globals::getInstance()->invalidateCallSites();
		SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
		s->mClassLevelMap[class_name] = level;
	}

	void setFileLevel(const std::string& file_name, ELevel level)
	{
		Globals::getInstance()->invalidateCallSites();
		SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
		s->mFileLevelMap[file_name] = level;
	}

	void setTagLevel(const std::string& tag_name, ELevel level)
	{
		Globals::getInstance()->invalidateCallSites();
		SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
		s->mTagLevelMap[tag_name] = level;
	}

	LLError::ELevel decodeLevel(std::string name)
	{
		static LevelMap level_names;
		if (level_names.empty())
		{
			level_names["ALL"]		= LLError::LEVEL_ALL;
			level_names["DEBUG"]	= LLError::LEVEL_DEBUG;
			level_names["INFO"]		= LLError::LEVEL_INFO;
			level_names["WARN"]		= LLError::LEVEL_WARN;
			level_names["ERROR"]	= LLError::LEVEL_ERROR;
			level_names["NONE"]		= LLError::LEVEL_NONE;
		}
		
		std::transform(name.begin(), name.end(), name.begin(), toupper);
		
		LevelMap::const_iterator i = level_names.find(name);
		if (i == level_names.end())
		{
			LL_WARNS() << "unrecognized logging level: '" << name << "'" << LL_ENDL;
			return LLError::LEVEL_INFO;
		}
		
		return i->second;
	}
}

namespace {
	void setLevels(LevelMap& map, const LLSD& list, LLError::ELevel level)
	{
		for (const auto& i : list.array())
		{
			map[i.asString()] = level;
		}
	}
}

namespace LLError
{
	void configure(const LLSD& config)
	{
		Globals::getInstance()->invalidateCallSites();
		SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
		
		s->mFunctionLevelMap.clear();
		s->mClassLevelMap.clear();
		s->mFileLevelMap.clear();
		s->mTagLevelMap.clear();
		s->mUniqueLogMessages.clear();
		
		setDefaultLevel(decodeLevel(config["default-level"]));
        if (config.has("log-always-flush"))
        {
            setAlwaysFlush(config["log-always-flush"]);
        }
        if (config.has("enabled-log-types-mask"))
        {
            setEnabledLogTypesMask(config["enabled-log-types-mask"].asInteger());
        }
        
        if (config.has("settings") && config["settings"].isArray())
        {
            LLSD sets = config["settings"];
            LLSD::array_const_iterator a, end;
            for (a = sets.beginArray(), end = sets.endArray(); a != end; ++a)
            {
                const LLSD& entry = *a;
                if (entry.isMap() && entry.size() != 0)
                {
                    ELevel level = decodeLevel(entry["level"]);

                    setLevels(s->mFunctionLevelMap, entry["functions"], level);
                    setLevels(s->mClassLevelMap, entry["classes"], level);
                    setLevels(s->mFileLevelMap, entry["files"], level);
                    setLevels(s->mTagLevelMap, entry["tags"], level);
                }
            }
        }
	}
}


namespace LLError
{
	Recorder::Recorder()
    	: mWantsTime(true)
        , mWantsTags(true)
        , mWantsLevel(true)
        , mWantsLocation(true)
        , mWantsFunctionName(true)
        , mWantsMultiline(false)
	{
	}

	bool Recorder::wantsTime()
	{ 
		return mWantsTime; 
	}

	// virtual
	bool Recorder::wantsTags()
	{
		return mWantsTags;
	}

	// virtual 
	bool Recorder::wantsLevel() 
	{ 
		return mWantsLevel;
	}

	// virtual 
	bool Recorder::wantsLocation() 
	{ 
		return mWantsLocation;
	}

	// virtual 
	bool Recorder::wantsFunctionName() 
	{ 
		return mWantsFunctionName;
	}

	// virtual 
	bool Recorder::wantsMultiline() 
	{ 
		return mWantsMultiline;
	}

    void Recorder::showTime(bool show)
    {
        mWantsTime = show;
    }
    
    void Recorder::showTags(bool show)
    {
        mWantsTags = show;
    }

    void Recorder::showLevel(bool show)
    {
        mWantsLevel = show;
    }

    void Recorder::showLocation(bool show)
    {
        mWantsLocation = show;
    }

    void Recorder::showFunctionName(bool show)
    {
        mWantsFunctionName = show;
    }

    void Recorder::showMultiline(bool show)
    {
        mWantsMultiline = show;
    }

	void addRecorder(RecorderPtr recorder)
	{
		if (!recorder)
		{
			return;
		}
		SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
		s->mRecorders.emplace_back(std::move(recorder));
	}

	void removeRecorder(RecorderPtr recorder)
	{
		if (!recorder)
		{
			return;
		}
		SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
		s->mRecorders.erase(std::remove(s->mRecorders.begin(), s->mRecorders.end(), recorder),
							s->mRecorders.end());
	}

    // Find an entry in SettingsConfig::mRecorders whose RecorderPtr points to
    // a Recorder subclass of type RECORDER. Return, not a RecorderPtr (which
    // points to the Recorder base class), but a shared_ptr<RECORDER> which
    // specifically points to the concrete RECORDER subclass instance, along
    // with a Recorders::iterator indicating the position of that entry in
    // mRecorders. The shared_ptr might be empty (operator!() returns true) if
    // there was no such RECORDER subclass instance in mRecorders.
    template <typename RECORDER>
    std::pair<boost::shared_ptr<RECORDER>, Recorders::iterator>
    findRecorderPos()
    {
        SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
        // Since we promise to return an iterator, use a classic iterator
        // loop.
        auto end{s->mRecorders.end()};
        for (Recorders::iterator it{s->mRecorders.begin()}; it != end; ++it)
        {
            // *it is a RecorderPtr, a shared_ptr<Recorder>. Use a
            // dynamic_pointer_cast to try to downcast to test if it's also a
            // shared_ptr<RECORDER>.
            auto ptr = boost::dynamic_pointer_cast<RECORDER>(*it);
            if (ptr)
            {
                // found the entry we want
                return { ptr, it };
            }
        }
        // dropped out of the loop without finding any such entry -- instead
        // of default-constructing Recorders::iterator (which might or might
        // not be valid), return a value that is valid but not dereferenceable.
        return { {}, end };
    }

    // Find an entry in SettingsConfig::mRecorders whose RecorderPtr points to
    // a Recorder subclass of type RECORDER. Return, not a RecorderPtr (which
    // points to the Recorder base class), but a shared_ptr<RECORDER> which
    // specifically points to the concrete RECORDER subclass instance. The
    // shared_ptr might be empty (operator!() returns true) if there was no
    // such RECORDER subclass instance in mRecorders.
    template <typename RECORDER>
    boost::shared_ptr<RECORDER> findRecorder()
    {
        return findRecorderPos<RECORDER>().first;
    }

    // Remove an entry from SettingsConfig::mRecorders whose RecorderPtr
    // points to a Recorder subclass of type RECORDER. Return true if there
    // was one and we removed it, false if there wasn't one to start with.
    template <typename RECORDER>
    bool removeRecorder()
    {
        auto found = findRecorderPos<RECORDER>();
        if (found.first)
        {
            SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
            s->mRecorders.erase(found.second);
        }
        return bool(found.first);
    }
}

namespace LLError
{
	void logToFile(const std::string& file_name)
	{
		// remove any previous Recorder filling this role
		removeRecorder<RecordToFile>();

		if (!file_name.empty())
		{
			boost::shared_ptr<RecordToFile> recordToFile = boost::make_shared<RecordToFile>(file_name);
			if (recordToFile->okay())
			{
				addRecorder(std::move(recordToFile));
			}
		}
	}

	std::string logFileName()
	{
		auto found = findRecorder<RecordToFile>();
		return found? found->getFilename() : std::string();
	}

    void logToStderr()
    {
        if (! findRecorder<RecordToStderr>())
        {
			RecorderPtr recordToStdErr = boost::make_shared<RecordToStderr>(stderrLogWantsTime());
            addRecorder(std::move(recordToStdErr));
        }
    }

	void logToFixedBuffer(LLLineBuffer* fixedBuffer)
	{
		// remove any previous Recorder filling this role
		removeRecorder<RecordToFixedBuffer>();

		if (fixedBuffer)
		{
			RecorderPtr recordToFixedBuffer = boost::make_shared<RecordToFixedBuffer>(fixedBuffer);
			addRecorder(std::move(recordToFixedBuffer));
		}
	}
}

namespace
{
    std::string escapedMessageLines(const std::string& message)
    {
        std::ostringstream out;
        size_t written_out = 0;
        size_t all_content = message.length();
        size_t escape_char_index; // always relative to start of message
        // Use find_first_of to find the next character in message that needs escaping
        for ( escape_char_index = message.find_first_of("\\\n\r");
              escape_char_index != std::string::npos && written_out < all_content;
              // record what we've written this iteration, scan for next char that needs escaping
              written_out = escape_char_index + 1, escape_char_index = message.find_first_of("\\\n\r", written_out)
             )
        {
            // found a character that needs escaping, so write up to that with the escape prefix
            // note that escape_char_index is relative to the start, not to the written_out offset
            out << message.substr(written_out, escape_char_index - written_out) << '\\';

            // write out the appropriate second character in the escape sequence
            char found = message[escape_char_index];
            switch ( found )
            {
            case '\\':
                out << '\\';
                break;
            case '\n':
                out << 'n';
                break;
            case '\r':
                out << 'r';
                break;
            }
        }

        if ( written_out < all_content ) // if the loop above didn't write everything
        {
            // write whatever was left
            out << message.substr(written_out, std::string::npos);
        }
        return out.str();
    }

	void writeToRecorders(const LLError::CallSite& site, const std::string& message)
	{
		LLError::ELevel level = site.mLevel;
		LLError::SettingsConfigPtr s = LLError::Settings::getInstance()->getSettingsConfig();

        std::string escaped_message;
        
		for (Recorders::const_iterator i = s->mRecorders.begin();
			i != s->mRecorders.end();
			++i)
		{
			LLError::RecorderPtr r = *i;

            if (!r->enabled())
            {
                continue;
            }
            
			std::ostringstream message_stream;

			if (r->wantsTime() && s->mTimeFunction != NULL)
			{
				message_stream << s->mTimeFunction();
			}
            message_stream << " ";
            
			if (r->wantsLevel())
            {
				message_stream << site.mLevelString;
            }
            message_stream << " ";
				
			if (r->wantsTags())
			{
				message_stream << site.mTagString;
			}
            message_stream << " ";

            if (r->wantsLocation() || level == LLError::LEVEL_ERROR)
            {
                message_stream << site.mLocationString;
            }
            message_stream << " ";

			if (r->wantsFunctionName())
			{
				message_stream << site.mFunctionString;
			}
            message_stream << " : ";

            if (r->wantsMultiline())
            {
                message_stream << message;
            }
            else
            {
                if (escaped_message.empty())
                {
                    escaped_message = escapedMessageLines(message);
                }
                message_stream << escaped_message;
            }

			r->recordMessage(level, message_stream.str());
		}
	}
}

namespace {
	// We need a couple different mutexes, but we want to use the same mechanism
	// for both. Make getMutex() a template function with different instances
	// for different MutexDiscriminator values.
	enum MutexDiscriminator
	{
		LOG_MUTEX,
		STACKS_MUTEX
	};

	ABSL_CONST_INIT absl::Mutex sLogMutex(absl::kConstInit);
	ABSL_CONST_INIT absl::Mutex sStackMutex(absl::kConstInit);

	template <MutexDiscriminator MTX>
	absl::Mutex* getMutex()
	{
		return nullptr;
	}

	template <>
	absl::Mutex* getMutex<LOG_MUTEX>()
	{
		return &sLogMutex;
	}

	template <>
	absl::Mutex* getMutex<STACKS_MUTEX>()
	{
		return &sStackMutex;
	}

	bool checkLevelMap(const LevelMap& map, const std::string& key,
						LLError::ELevel& level)
	{
		LevelMap::const_iterator i = map.find(key);
		if (i == map.end())
		{
			return false;
		}
		
			level = i->second;
		return true;
	}
	
	bool checkLevelMap(	const LevelMap& map, 
						const char *const * keys, 
						size_t count,
						LLError::ELevel& level)
	{
		bool found_level = false;

		LLError::ELevel tag_level = LLError::LEVEL_NONE;

		for (size_t i = 0; i < count; i++)
		{
			LevelMap::const_iterator it = map.find(keys[i]);
			if (it != map.end())
			{
				found_level = true;
				tag_level = llmin(tag_level, it->second);
			}
		}

		if (found_level)
		{
			level = tag_level;
		}
		return found_level;
	}
}

namespace LLError
{

	bool Log::shouldLog(CallSite& site)
	{
		AbslMutexMaybeTrylock lock(getMutex<LOG_MUTEX>(), 5);
		if (!lock.isLocked())
		{
			return false;
		}

		SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
		
		s->mShouldLogCallCounter++;
		
		const std::string& class_name = className(site.mClassInfo);
		std::string function_name = functionName(site.mFunction);
#if LL_LINUX
		// gross, but typeid comparison seems to always fail here with gcc4.1
		if (0 != strcmp(site.mClassInfo.name(), typeid(NoClassInfo).name()))
#else
		if (site.mClassInfo != typeid(NoClassInfo))
#endif // LL_LINUX
		{
			function_name = absl::StrCat(class_name, "::", function_name);
		}

		ELevel compareLevel = s->mDefaultLevel;

		// The most specific match found will be used as the log level,
		// since the computation short circuits.
		// So, in increasing order of importance:
		// Default < Tags < File < Class < Function
		checkLevelMap(s->mFunctionLevelMap, function_name, compareLevel)
		|| checkLevelMap(s->mClassLevelMap, class_name, compareLevel)
		|| checkLevelMap(s->mFileLevelMap, abbreviateFile(site.mFile), compareLevel)
		|| (site.mTagCount > 0
			? checkLevelMap(s->mTagLevelMap, site.mTags, site.mTagCount, compareLevel) 
			: false);

		site.mCached = true;
		Globals::getInstance()->addCallSite(site);
		return site.mShouldLog = site.mLevel >= compareLevel;
	}


	std::ostringstream* Log::out()
	{
		AbslMutexMaybeTrylock lock(getMutex<LOG_MUTEX>(),5);

		if (lock.isLocked())
		{
			Globals* g = Globals::getInstance();

			if (!g->messageStreamInUse)
			{
				g->messageStreamInUse = true;
				return &g->messageStream;
			}
		}

		return new std::ostringstream;
	}

	void Log::flush(std::ostringstream* out, char* message)
	{
		AbslMutexMaybeTrylock lock(getMutex<LOG_MUTEX>(),5);
		if (!lock.isLocked())
		{
			return;
		}

		if(strlen(out->str().c_str()) < 128)
		{
			strcpy(message, out->str().c_str());
		}
		else
		{
			strncpy(message, out->str().c_str(), 127);
			message[127] = '\0' ;
		}

		Globals* g = Globals::getInstance();
		if (out == &g->messageStream)
		{
			g->messageStream.clear();
			g->messageStream.str("");
			g->messageStreamInUse = false;
		}
		else
		{
			delete out;
		}
		return ;
	}

	void Log::flush(std::ostringstream* out, const CallSite& site)
	{
		AbslMutexMaybeTrylock lock(getMutex<LOG_MUTEX>(),5);
		if (!lock.isLocked())
		{
			return;
		}

		Globals* g = Globals::getInstance();
		SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();

		std::string message = out->str();
		if (out == &g->messageStream)
		{
			g->messageStream.clear();
			g->messageStream.str("");
			g->messageStreamInUse = false;
		}
		else
		{
			delete out;
		}


		if (site.mPrintOnce)
		{
            std::ostringstream message_stream;

			auto messageIter = s->mUniqueLogMessages.find(message);
			if (messageIter != s->mUniqueLogMessages.end())
			{
				messageIter->second++;
				unsigned int num_messages = messageIter->second;
				if (num_messages == 10 || num_messages == 50 || (num_messages % 100) == 0)
				{
					message_stream << "ONCE (" << num_messages << "th time seen): ";
				} 
				else
				{
					return;
				}
			}
			else 
			{
				message_stream << "ONCE: ";
				s->mUniqueLogMessages[message] = 1;
			}
            message_stream << message;
            message = message_stream.str();
		}
		
		writeToRecorders(site, message);

		if (site.mLevel == LEVEL_ERROR)
		{
			g->mFatalMessage = message;
			if (s->mCrashFunction)
			{
				s->mCrashFunction(message);
			}
		}
	}
}

namespace LLError
{
	SettingsStoragePtr saveAndResetSettings()
	{
		return Settings::getInstance()->saveAndReset();
	}
	
	void restoreSettings(SettingsStoragePtr pSettingsStorage)
	{
		return Settings::getInstance()->restore(pSettingsStorage);
	}

	std::string removePrefix(std::string& s, const std::string& p)
	{
		std::string::size_type where = s.find(p);
		if (where == std::string::npos)
		{
			return s;
		}
		
		return std::string(s, where + p.size());
	}
	
	void replaceChar(std::string& s, char old, char replacement)
	{
		std::string::size_type i = 0;
		std::string::size_type len = s.length();
		for ( ; i < len; i++ )
		{
			if (s[i] == old)
			{
				s[i] = replacement;
			}
		}
	}

	std::string abbreviateFile(std::string f)
	{
#if LL_WINDOWS
		replaceChar(f, '\\', '/');
#endif
		static std::string indra_prefix = "indra/";
		f = removePrefix(f, indra_prefix);

#if LL_DARWIN
		static std::string newview_prefix = "newview/../";
		f = removePrefix(f, newview_prefix);
#endif
		return f;
	}

	int shouldLogCallCount()
	{
		SettingsConfigPtr s = Settings::getInstance()->getSettingsConfig();
		return s->mShouldLogCallCounter;
	}

#if LL_WINDOWS
		// VC80 was optimizing the error away.
		#pragma optimize("", off)
#endif
	void crashAndLoop(const std::string& message)
	{
		// Now, we go kaboom!
		int* make_me_crash = NULL;

		*make_me_crash = 0;

		while(true)
		{
			// Loop forever, in case the crash didn't work?
		}
		
		// this is an attempt to let Coverity and other semantic scanners know that this function won't be returning ever.
		exit(EXIT_FAILURE);
	}
#if LL_WINDOWS
		#pragma optimize("", on)
#endif

	std::string utcTime()
	{
		time_t now = time(NULL);
		const size_t BUF_SIZE = 64;
		char time_str[BUF_SIZE];	/* Flawfinder: ignore */
		
		int chars = strftime(time_str, BUF_SIZE, 
								  "%Y-%m-%dT%H:%M:%SZ",
								  gmtime(&now));

		return chars ? time_str : "time error";
	}
}

namespace LLError
{     
	char** LLCallStacks::sBuffer = NULL ;
	S32    LLCallStacks::sIndex  = 0 ;

	//static
    void LLCallStacks::allocateStackBuffer()
    {
        if(sBuffer == NULL)
        {
            sBuffer = new char*[512] ;
            sBuffer[0] = new char[512 * 128] ;
            for(S32 i = 1 ; i < 512 ; i++)
            {
                sBuffer[i] = sBuffer[i-1] + 128 ;
            }
            sIndex = 0 ;
        }
    }

    void LLCallStacks::freeStackBuffer()
    {
        if(sBuffer != NULL)
        {
            delete [] sBuffer[0] ;
            delete [] sBuffer ;
            sBuffer = NULL ;
        }
    }

    //static
    void LLCallStacks::push(const char* function, const int line)
    {
		AbslMutexMaybeTrylock lock(getMutex<STACKS_MUTEX>(), 5);
        if (!lock.isLocked())
        {
            return;
        }

        if(sBuffer == NULL)
        {
            allocateStackBuffer();
        }

        if(sIndex > 511)
        {
            clear() ;
        }

        strcpy(sBuffer[sIndex], function) ;
        sprintf(sBuffer[sIndex] + strlen(function), " line: %d ", line) ;
        sIndex++ ;

        return ;
    }

    //static
    std::ostringstream* LLCallStacks::insert(const char* function, const int line)
    {
        std::ostringstream* _out = LLError::Log::out();
        *_out << function << " line " << line << " " ;
        return _out ;
    }

    //static
    void LLCallStacks::end(std::ostringstream* _out)
    {
		AbslMutexMaybeTrylock lock(getMutex<STACKS_MUTEX>(), 5);
        if (!lock.isLocked())
        {
            return;
        }

        if(sBuffer == NULL)
        {
            allocateStackBuffer();
        }

        if(sIndex > 511)
        {
            clear() ;
        }

        LLError::Log::flush(_out, sBuffer[sIndex++]) ;
    }

    //static
    void LLCallStacks::print()
    {
		AbslMutexMaybeTrylock lock(getMutex<STACKS_MUTEX>(), 5);
        if (!lock.isLocked())
        {
            return;
        }

        if(sIndex > 0)
        {
            LL_INFOS() << " ************* PRINT OUT LL CALL STACKS ************* " << LL_ENDL;
            while(sIndex > 0)
            {                  
                sIndex-- ;
                LL_INFOS() << sBuffer[sIndex] << LL_ENDL;
            }
            LL_INFOS() << " *************** END OF LL CALL STACKS *************** " << LL_ENDL;
        }

        if(sBuffer != NULL)
        {
            freeStackBuffer();
        }
    }

    //static
    void LLCallStacks::clear()
    {
        sIndex = 0 ;
    }

    //static
    void LLCallStacks::cleanup()
    {
        freeStackBuffer();
    }

    std::ostream& operator<<(std::ostream& out, const LLStacktrace&)
    {
        return out << boost::stacktrace::stacktrace();
    }
}

bool debugLoggingEnabled(const std::string& tag)
{
	AbslMutexMaybeTrylock lock(getMutex<LOG_MUTEX>(), 5);
    if (!lock.isLocked())
    {
        return false;
    }
        
    LLError::SettingsConfigPtr s = LLError::Settings::getInstance()->getSettingsConfig();
    LLError::ELevel level = LLError::LEVEL_DEBUG;
    bool res = checkLevelMap(s->mTagLevelMap, tag, level);
    return res;
}



