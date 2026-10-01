#pragma once

// Minimal, self-contained replacement for the subset of CLI11 that Quickshell's command line
// parser (src/launch/parsecommand.cpp) uses. It exists so the Windows cross build can be made
// without network access; it is only used with -DBUNDLED_CLI11=ON and the real CLI11 should be
// preferred whenever it is available (find_package or CLI11_INCLUDE_DIR).
//
// Supported: options (string/integer/vector targets), flags (bool / count callback), positionals,
// subcommands, option groups, --help, environment fallbacks (envname), excludes/needs, Range
// validators, require_subcommand / require_option and CLI11_PARSE. Unsupported CLI11 features
// are simply absent, so using one is a compile error rather than a silent behavior change.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace CLI {

// ---------------------------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------------------------

class Error: public std::runtime_error {
public:
	Error(std::string name, const std::string& message, int exitCode = 1)
	    : std::runtime_error(message)
	    , name_(std::move(name))
	    , exitCode_(exitCode) {}

	[[nodiscard]] int get_exit_code() const { return this->exitCode_; }
	[[nodiscard]] const std::string& get_name() const { return this->name_; }

private:
	std::string name_;
	int exitCode_;
};

class ParseError: public Error {
public:
	using Error::Error;
};

class CallForHelp: public ParseError {
public:
	CallForHelp(): ParseError("CallForHelp", "This should be caught in your main function", 0) {}
};

#define CLI11_FALLBACK_ERROR(Name, Code)                                                           \
	class Name: public ParseError {                                                                  \
	public:                                                                                          \
		explicit Name(const std::string& message): ParseError(#Name, message, Code) {}                 \
	};

CLI11_FALLBACK_ERROR(ConversionError, 102)
CLI11_FALLBACK_ERROR(ValidationError, 103)
CLI11_FALLBACK_ERROR(RequiredError, 104)
CLI11_FALLBACK_ERROR(RequiresError, 105)
CLI11_FALLBACK_ERROR(ExcludesError, 106)
CLI11_FALLBACK_ERROR(ExtrasError, 107)
CLI11_FALLBACK_ERROR(ArgumentMismatch, 109)
CLI11_FALLBACK_ERROR(HorribleError, 110)

#undef CLI11_FALLBACK_ERROR

// ---------------------------------------------------------------------------------------------
// Validators
// ---------------------------------------------------------------------------------------------

class Validator {
public:
	Validator() = default;
	Validator(std::function<std::string(const std::string&)> func, std::string description)
	    : func_(std::move(func))
	    , description_(std::move(description)) {}

	// Returns an error message, or an empty string if the value is acceptable.
	[[nodiscard]] std::string operator()(const std::string& value) const {
		return this->func_ ? this->func_(value) : std::string();
	}

	[[nodiscard]] const std::string& get_description() const { return this->description_; }

private:
	std::function<std::string(const std::string&)> func_;
	std::string description_;
};

namespace detail {

inline bool parseInteger(const std::string& text, long long& out) {
	if (text.empty()) return false;
	char* end = nullptr;
	errno = 0;
	out = std::strtoll(text.c_str(), &end, 10);
	return errno == 0 && end != nullptr && *end == '\0';
}

inline bool parseDouble(const std::string& text, double& out) {
	if (text.empty()) return false;
	char* end = nullptr;
	out = std::strtod(text.c_str(), &end);
	return end != nullptr && *end == '\0';
}

inline bool looksLikeNumber(const std::string& text) {
	double value = 0;
	return parseDouble(text, value);
}

template <typename T>
std::string numberToString(T value) {
	std::ostringstream stream;
	stream << value;
	return stream.str();
}

inline std::string programName(const std::string& argv0) {
	auto pos = argv0.find_last_of("/\\");
	return pos == std::string::npos ? argv0 : argv0.substr(pos + 1);
}

} // namespace detail

class Range: public Validator {
public:
	template <typename T>
	Range(T min, T max, const std::string& name = "")
	    : Validator(
	          [min, max](const std::string& text) -> std::string {
		          auto fail = [&]() {
			          return "Value " + text + " not in range [" + detail::numberToString(min) + " - "
			               + detail::numberToString(max) + "]";
		          };

		          if constexpr (std::is_integral_v<T>) {
			          long long value = 0;
			          if (!detail::parseInteger(text, value)) return fail();
			          if (value < static_cast<long long>(min) || value > static_cast<long long>(max)) {
				          return fail();
			          }
		          } else {
			          double value = 0;
			          if (!detail::parseDouble(text, value)) return fail();
			          if (value < static_cast<double>(min) || value > static_cast<double>(max)) {
				          return fail();
			          }
		          }

		          return std::string();
	          },
	          name.empty() ? (std::is_integral_v<T> ? "INT" : "FLOAT") + std::string(" in [")
	                             + detail::numberToString(min) + " - "
	                             + detail::numberToString(max) + "]"
	                       : name
	      ) {}
};

// ---------------------------------------------------------------------------------------------
// Value conversion
// ---------------------------------------------------------------------------------------------

namespace detail {

template <typename T>
struct IsVector: std::false_type {};

template <typename T, typename A>
struct IsVector<std::vector<T, A>>: std::true_type {};

template <typename T>
void assignValue(T& target, const std::string& text, const std::string& optionName) {
	if constexpr (std::is_same_v<T, bool>) {
		if (text == "true" || text == "1" || text == "yes" || text == "on") target = true;
		else if (text == "false" || text == "0" || text == "no" || text == "off") target = false;
		else throw ConversionError("The value " + text + " is not a boolean for " + optionName);
	} else if constexpr (std::is_integral_v<T>) {
		long long value = 0;
		if (!parseInteger(text, value) || value < std::numeric_limits<T>::lowest()
		    || value > std::numeric_limits<T>::max())
		{
			throw ConversionError("The value " + text + " is not an integer for " + optionName);
		}
		target = static_cast<T>(value);
	} else if constexpr (std::is_floating_point_v<T>) {
		double value = 0;
		if (!parseDouble(text, value)) {
			throw ConversionError("The value " + text + " is not a number for " + optionName);
		}
		target = static_cast<T>(value);
	} else {
		// std::string, or anything assignable from one (e.g. QStringOption).
		target = text;
	}
}

template <typename T>
constexpr const char* typeName() {
	if constexpr (std::is_same_v<T, bool>) return "BOOLEAN";
	else if constexpr (std::is_integral_v<T>) return "INT";
	else if constexpr (std::is_floating_point_v<T>) return "FLOAT";
	else return "TEXT";
}

} // namespace detail

// ---------------------------------------------------------------------------------------------
// Option
// ---------------------------------------------------------------------------------------------

class App;

class Option {
public:
	Option* description(std::string description) {
		this->description_ = std::move(description);
		return this;
	}

	Option* envname(std::string name) {
		this->envname_ = std::move(name);
		return this;
	}

	Option* excludes(Option* other) {
		this->excludes_.push_back(other);
		return this;
	}

	Option* needs(Option* other) {
		this->needs_.push_back(other);
		return this;
	}

	Option* check(Validator validator) {
		this->validators_.push_back(std::move(validator));
		return this;
	}

	// Lets a (vector) option accept every remaining value.
	Option* allow_extra_args(bool allow = true) {
		this->allowExtraArgs_ = allow;
		if (allow) this->expected_ = -1;
		return this;
	}

	Option* expected(int count) {
		this->expected_ = count;
		return this;
	}

	[[nodiscard]] std::size_t count() const { return this->count_; }
	[[nodiscard]] bool get_positional() const { return this->positional_; }
	[[nodiscard]] const std::string& get_description() const { return this->description_; }

	// Name used in messages and help: the first long name, then short, then positional.
	[[nodiscard]] std::string get_name() const {
		if (!this->longNames_.empty()) return "--" + this->longNames_.front();
		if (!this->shortNames_.empty()) return "-" + this->shortNames_.front();
		return this->positionalName_;
	}

	[[nodiscard]] std::string get_display_name() const {
		std::string out;
		for (const auto& name: this->shortNames_) out += (out.empty() ? "" : ",") + ("-" + name);
		for (const auto& name: this->longNames_) out += (out.empty() ? "" : ",") + ("--" + name);
		if (this->positional_) out = this->positionalName_;
		if (!this->flag_) out += " " + this->typeName_ + (this->expected_ == -1 ? " ..." : "");
		for (const auto& validator: this->validators_) {
			if (!validator.get_description().empty()) out += ":" + validator.get_description();
		}
		if (!this->envname_.empty()) out += " (Env:" + this->envname_ + ")";
		return out;
	}

private:
	friend class App;

	Option(const std::string& spec, bool flag) : flag_(flag) {
		std::size_t start = 0;
		while (start <= spec.size()) {
			auto end = spec.find(',', start);
			if (end == std::string::npos) end = spec.size();
			auto token = spec.substr(start, end - start);
			start = end + 1;

			// trim
			while (!token.empty() && token.front() == ' ') token.erase(0, 1);
			while (!token.empty() && token.back() == ' ') token.pop_back();
			if (token.empty()) continue;

			if (token.rfind("--", 0) == 0) this->longNames_.push_back(token.substr(2));
			else if (token.front() == '-') this->shortNames_.push_back(token.substr(1));
			else this->positionalName_ = token;
		}

		this->positional_ = this->longNames_.empty() && this->shortNames_.empty();
	}

	[[nodiscard]] bool hasLong(const std::string& name) const {
		return std::find(this->longNames_.begin(), this->longNames_.end(), name)
		    != this->longNames_.end();
	}

	[[nodiscard]] bool hasShort(char name) const {
		return std::find(this->shortNames_.begin(), this->shortNames_.end(), std::string(1, name))
		    != this->shortNames_.end();
	}

	[[nodiscard]] bool acceptsMore() const {
		return this->expected_ == -1 || this->count_ < static_cast<std::size_t>(this->expected_);
	}

	void addResult(const std::string& value) {
		this->results_.push_back(value);
		this->count_++;
	}

	void finalize() {
		if (this->count_ == 0 && !this->envname_.empty()) {
			const auto* env = std::getenv(this->envname_.c_str()); // NOLINT
			if (env != nullptr && *env != '\0') this->addResult(env);
		}

		if (this->count_ == 0) return;

		if (!this->flag_) {
			for (const auto& value: this->results_) {
				for (const auto& validator: this->validators_) {
					auto error = validator(value);
					if (!error.empty()) throw ValidationError(this->get_name() + ": " + error);
				}
			}
		}

		if (this->valueCallback_) this->valueCallback_(this->results_);
		if (this->countCallback_) this->countCallback_(this->count_);
	}

	void checkRelations() const {
		if (this->count_ == 0) return;

		for (const auto* other: this->excludes_) {
			if (other->count_ > 0) {
				throw ExcludesError(this->get_name() + " excludes " + other->get_name());
			}
		}

		for (const auto* other: this->needs_) {
			if (other->count_ == 0) {
				throw RequiresError(this->get_name() + " requires " + other->get_name());
			}
		}
	}

	std::vector<std::string> shortNames_;
	std::vector<std::string> longNames_;
	std::string positionalName_;
	std::string description_;
	std::string envname_;
	std::string typeName_ = "TEXT";
	bool flag_ = false;
	bool positional_ = false;
	bool allowExtraArgs_ = false;
	int expected_ = 1;
	std::vector<Option*> excludes_;
	std::vector<Option*> needs_;
	std::vector<Validator> validators_;
	std::function<void(const std::vector<std::string>&)> valueCallback_;
	std::function<void(std::size_t)> countCallback_;
	std::vector<std::string> results_;
	std::size_t count_ = 0;
};

// ---------------------------------------------------------------------------------------------
// App (also used for subcommands and option groups)
// ---------------------------------------------------------------------------------------------

class App {
public:
	explicit App(std::string description = "", std::string name = "")
	    : name_(std::move(name))
	    , description_(std::move(description)) {
		this->helpFlag_ = this->addOption("-h,--help", true);
		this->helpFlag_->description("Print this help message and exit");
	}

	virtual ~App() = default;
	App(const App&) = delete;
	App& operator=(const App&) = delete;

	App* description(std::string description) {
		this->description_ = std::move(description);
		return this;
	}

	App* add_subcommand(std::string name, std::string description = "") {
		auto sub = std::unique_ptr<App>(new App(std::move(description), std::move(name)));
		sub->parent_ = this;
		this->subcommands_.push_back(std::move(sub));
		return this->subcommands_.back().get();
	}

	// Option groups have no help flag and are parsed as part of their parent.
	App* add_option_group(std::string name, std::string description = "") {
		auto group = std::unique_ptr<App>(new App(std::move(description), ""));
		group->parent_ = this;
		group->isGroup_ = true;
		group->groupName_ = std::move(name);
		group->options_.clear();
		group->helpFlag_ = nullptr;
		this->groups_.push_back(std::move(group));
		return this->groups_.back().get();
	}

	App* require_subcommand(std::size_t min = 1, std::size_t max = 0) {
		this->requireSubcommand_ = true;
		this->requireSubcommandMin_ = min;
		this->requireSubcommandMax_ = max;
		return this;
	}

	App* require_option(std::size_t min = 1, std::size_t max = 0) {
		this->requireOptionMin_ = min;
		this->requireOptionMax_ = max;
		return this;
	}

	App* excludes(Option* option) {
		this->excludedOptions_.push_back(option);
		return this;
	}

	App* excludes(App* group) {
		this->excludedGroups_.push_back(group);
		return this;
	}

	template <typename T>
	Option* add_option(const std::string& name, T& variable, std::string description = "") {
		auto* option = this->addOption(name, false);
		option->description(std::move(description));

		if constexpr (detail::IsVector<T>::value) {
			using Item = typename T::value_type;
			option->typeName_ = detail::typeName<Item>();
			option->expected_ = -1;
			option->valueCallback_ = [&variable, option](const std::vector<std::string>& values) {
				variable.clear();
				for (const auto& value: values) {
					Item item;
					detail::assignValue(item, value, option->get_name());
					variable.push_back(std::move(item));
				}
			};
		} else {
			option->typeName_ = detail::typeName<T>();
			option->valueCallback_ = [&variable, option](const std::vector<std::string>& values) {
				detail::assignValue(variable, values.back(), option->get_name());
			};
		}

		return option;
	}

	Option* add_flag(const std::string& name, bool& variable, std::string description = "") {
		auto* option = this->addOption(name, true);
		option->description(std::move(description));
		option->countCallback_ = [&variable](std::size_t) { variable = true; };
		return option;
	}

	Option* add_flag(
	    const std::string& name,
	    std::function<void(std::size_t)> callback,
	    std::string description = ""
	) {
		auto* option = this->addOption(name, true);
		option->description(std::move(description));
		option->countCallback_ = std::move(callback);
		return option;
	}

	[[nodiscard]] const std::string& get_name() const { return this->name_; }
	[[nodiscard]] const std::string& get_description() const { return this->description_; }
	[[nodiscard]] std::size_t count() const { return this->parsed_; }
	explicit operator bool() const { return this->parsed_ > 0; }

	// Number of option/positional occurrences in this app, its groups and parsed subcommands.
	[[nodiscard]] std::size_t count_all() const {
		std::size_t total = 0;
		for (const auto& option: this->options_) {
			if (option.get() != this->helpFlag_) total += option->count();
		}
		for (const auto& group: this->groups_) total += group->count_all();
		for (const auto& sub: this->subcommands_) total += sub->count_all();
		if (!this->name_.empty()) total += this->parsed_;
		return total;
	}

	[[nodiscard]] std::vector<App*> get_subcommands() const { return this->parsedSubcommands_; }

	void parse(int argc, const char* const* argv) {
		if (this->name_.empty() && argc > 0) this->name_ = detail::programName(argv[0]); // NOLINT
		std::vector<std::string> args;
		for (auto i = 1; i < argc; ++i) args.emplace_back(argv[i]); // NOLINT
		this->parse(args);
	}

	void parse(const std::vector<std::string>& args) {
		this->reset();
		this->parsed_++;

		auto* current = this;
		auto onlyPositionals = false;
		std::size_t i = 0;

		while (i < args.size()) {
			const auto& arg = args[i];

			if (!onlyPositionals && arg == "--") {
				onlyPositionals = true;
				++i;
				continue;
			}

			if (!onlyPositionals && arg.size() > 2 && arg.rfind("--", 0) == 0) {
				i = current->parseLong(args, i);
				continue;
			}

			if (!onlyPositionals && arg.size() > 1 && arg.front() == '-'
			    && !(detail::looksLikeNumber(arg) && current->findShort(arg[1]) == nullptr))
			{
				i = current->parseShort(args, i);
				continue;
			}

			if (!onlyPositionals) {
				if (auto* sub = current->findSubcommand(arg)) {
					sub->parsed_++;
					current->parsedSubcommands_.push_back(sub);
					current = sub;
					++i;
					continue;
				}
			}

			current->addPositional(arg);
			++i;
		}

		this->finalize();
	}

	int exit(const Error& error, std::ostream& out = std::cout, std::ostream& err = std::cerr) const {
		if (error.get_name() == "CallForHelp") {
			out << this->help();
			return error.get_exit_code();
		}

		if (error.get_exit_code() != 0) {
			err << error.get_name() << ": " << error.what() << '\n'
			    << "Run with --help for more information." << std::endl;
		}

		return error.get_exit_code();
	}

	[[nodiscard]] std::string help(const std::string& prev = "") const {
		auto fullName = prev.empty() ? this->name_ : prev + " " + this->name_;

		// Delegate to the selected subcommand, like CLI11 does.
		if (!this->parsedSubcommands_.empty()) return this->parsedSubcommands_.front()->help(fullName);

		std::ostringstream out;
		if (!this->description_.empty()) out << this->description_ << '\n';

		out << "Usage: " << fullName;
		if (!this->options_.empty() || !this->groups_.empty()) out << " [OPTIONS]";
		for (const auto& option: this->options_) {
			if (option->positional_) {
				out << ' ' << (option->expected_ == -1 ? "[" + option->positionalName_ + "...]"
				                                       : "[" + option->positionalName_ + "]");
			}
		}
		if (!this->subcommands_.empty()) {
			out << (this->requireSubcommand_ && this->requireSubcommandMin_ > 0 ? " SUBCOMMAND"
			                                                                      : " [SUBCOMMAND]");
		}
		out << "\n";

		auto printOptions = [&](const App& app, bool positionals) {
			for (const auto& option: app.options_) {
				if (option->positional_ != positionals) continue;
				App::formatOption(out, *option);
			}
		};

		auto hasPositionals = std::any_of(
		    this->options_.begin(),
		    this->options_.end(),
		    [](const std::unique_ptr<Option>& o) { return o->positional_; }
		);

		if (hasPositionals) {
			out << "\nPositionals:\n";
			printOptions(*this, true);
		}

		out << "\nOptions:\n";
		printOptions(*this, false);

		for (const auto& group: this->groups_) {
			if (group->groupName_.empty()) continue; // hidden group
			out << '\n' << group->groupName_ << ":\n";
			if (!group->description_.empty()) {
				std::istringstream lines(group->description_);
				std::string line;
				while (std::getline(lines, line)) out << "  " << line << '\n';
				out << '\n';
			}
			printOptions(*group, true);
			printOptions(*group, false);
		}

		if (!this->subcommands_.empty()) {
			out << "\nSubcommands:\n";
			for (const auto& sub: this->subcommands_) {
				App::formatEntry(out, sub->name_, sub->description_);
			}
		}

		return out.str();
	}

private:
	static constexpr std::size_t HELP_COLUMN = 30;

	static void formatEntry(std::ostream& out, const std::string& name, const std::string& desc) {
		std::istringstream lines(desc);
		std::string line;
		auto first = true;

		out << "  " << name;
		if (desc.empty()) {
			out << '\n';
			return;
		}

		while (std::getline(lines, line)) {
			if (first) {
				if (name.size() + 2 >= HELP_COLUMN) out << '\n' << std::string(HELP_COLUMN, ' ');
				else out << std::string(HELP_COLUMN - name.size() - 2, ' ');
				first = false;
			} else {
				out << std::string(HELP_COLUMN, ' ');
			}
			out << line << '\n';
		}
	}

	static void formatOption(std::ostream& out, const Option& option) {
		App::formatEntry(out, option.get_display_name(), option.description_);
	}

	Option* addOption(const std::string& spec, bool flag) {
		auto option = std::unique_ptr<Option>(new Option(spec, flag));
		this->options_.push_back(std::move(option));
		return this->options_.back().get();
	}

	void reset() {
		this->parsed_ = 0;
		this->parsedSubcommands_.clear();
		for (auto& option: this->options_) {
			option->results_.clear();
			option->count_ = 0;
		}
		for (auto& group: this->groups_) group->reset();
		for (auto& sub: this->subcommands_) sub->reset();
	}

	// Lookups search this app, its option groups, then the parent chain (lenient fallthrough).
	[[nodiscard]] Option* findLongLocal(const std::string& name) const {
		for (const auto& option: this->options_) {
			if (option->hasLong(name)) return option.get();
		}
		for (const auto& group: this->groups_) {
			if (auto* option = group->findLongLocal(name)) return option;
		}
		return nullptr;
	}

	[[nodiscard]] Option* findShortLocal(char name) const {
		for (const auto& option: this->options_) {
			if (option->hasShort(name)) return option.get();
		}
		for (const auto& group: this->groups_) {
			if (auto* option = group->findShortLocal(name)) return option;
		}
		return nullptr;
	}

	[[nodiscard]] Option* findLong(const std::string& name) const {
		for (const auto* app = this; app != nullptr; app = app->parent_) {
			if (auto* option = app->findLongLocal(name)) return option;
		}
		return nullptr;
	}

	[[nodiscard]] Option* findShort(char name) const {
		for (const auto* app = this; app != nullptr; app = app->parent_) {
			if (auto* option = app->findShortLocal(name)) return option;
		}
		return nullptr;
	}

	[[nodiscard]] App* findSubcommand(const std::string& name) const {
		for (const auto& sub: this->subcommands_) {
			if (sub->name_ == name) return sub.get();
		}
		return nullptr;
	}

	[[nodiscard]] Option* nextPositional() const {
		for (const auto& option: this->options_) {
			if (option->positional_ && option->acceptsMore()) return option.get();
		}
		for (const auto& group: this->groups_) {
			if (auto* option = group->nextPositional()) return option;
		}
		return nullptr;
	}

	void addPositional(const std::string& value) {
		auto* option = this->nextPositional();
		if (option == nullptr) {
			throw ExtrasError("The following argument was not expected: " + value);
		}
		option->addResult(value);
	}

	static void setOption(Option* option, const std::string& value) {
		if (option->flag_) {
			option->addResult("");
		} else {
			option->addResult(value);
		}
	}

	// Returns the index of the next unparsed argument.
	std::size_t parseLong(const std::vector<std::string>& args, std::size_t i) {
		const auto& arg = args[i];
		auto eq = arg.find('=');
		auto name = arg.substr(2, eq == std::string::npos ? std::string::npos : eq - 2);
		auto* option = this->findLong(name);

		if (option == nullptr) {
			throw ExtrasError("The following argument was not expected: " + arg);
		}

		if (option == this->findHelp()) throw CallForHelp();

		if (option->flag_) {
			App::setOption(option, "");
			return i + 1;
		}

		if (eq != std::string::npos) {
			App::setOption(option, arg.substr(eq + 1));
			return i + 1;
		}

		if (i + 1 >= args.size()) {
			throw ArgumentMismatch(option->get_name() + ": 1 required argument missing");
		}

		App::setOption(option, args[i + 1]);
		return i + 2;
	}

	std::size_t parseShort(const std::vector<std::string>& args, std::size_t i) {
		const auto& arg = args[i];

		for (std::size_t pos = 1; pos < arg.size(); ++pos) {
			auto* option = this->findShort(arg[pos]);

			if (option == nullptr) {
				throw ExtrasError("The following argument was not expected: -" + arg.substr(pos));
			}

			if (option == this->findHelp()) throw CallForHelp();

			if (option->flag_) {
				App::setOption(option, "");
				continue;
			}

			if (pos + 1 < arg.size()) {
				App::setOption(option, arg.substr(pos + 1));
				return i + 1;
			}

			if (i + 1 >= args.size()) {
				throw ArgumentMismatch(option->get_name() + ": 1 required argument missing");
			}

			App::setOption(option, args[i + 1]);
			return i + 2;
		}

		return i + 1;
	}

	[[nodiscard]] Option* findHelp() const {
		for (const auto* app = this; app != nullptr; app = app->parent_) {
			if (app->helpFlag_ != nullptr) return app->helpFlag_;
		}
		return nullptr;
	}

	// Processes env fallbacks, validators, callbacks and requirements of this app and its parsed
	// subcommands.
	void finalize() {
		for (auto& option: this->options_) option->finalize();
		for (auto& group: this->groups_) {
			for (auto& option: group->options_) option->finalize();
		}

		for (auto& option: this->options_) option->checkRelations();
		for (auto& group: this->groups_) {
			for (auto& option: group->options_) option->checkRelations();
			group->checkExcludes();
		}
		this->checkExcludes();

		if (this->requireSubcommand_) {
			auto count = this->parsedSubcommands_.size();
			if (count < this->requireSubcommandMin_) {
				throw RequiredError("A subcommand is required");
			}
			if (this->requireSubcommandMax_ > 0 && count > this->requireSubcommandMax_) {
				throw ExtrasError("Too many subcommands given");
			}
		}

		if (this->requireOptionMin_ > 0 || this->requireOptionMax_ > 0) {
			auto count = this->count_all() - (this->name_.empty() ? 0 : this->parsed_);
			if (count < this->requireOptionMin_) {
				throw RequiredError(
				    "At least " + std::to_string(this->requireOptionMin_) + " option(s) required"
				);
			}
			if (this->requireOptionMax_ > 0 && count > this->requireOptionMax_) {
				throw ExtrasError(
				    "At most " + std::to_string(this->requireOptionMax_) + " option(s) allowed"
				);
			}
		}

		for (auto* sub: this->parsedSubcommands_) sub->finalize();
	}

	void checkExcludes() const {
		if (this->count_all() == 0) return;

		for (const auto* option: this->excludedOptions_) {
			if (option->count() > 0) {
				throw ExcludesError(this->displayName() + " excludes " + option->get_name());
			}
		}

		for (const auto* group: this->excludedGroups_) {
			if (group->count_all() > 0) {
				throw ExcludesError(this->displayName() + " excludes " + group->displayName());
			}
		}
	}

	[[nodiscard]] std::string displayName() const {
		if (this->isGroup_) return this->groupName_.empty() ? "option group" : this->groupName_;
		return this->name_;
	}

	std::string name_;
	std::string description_;
	std::string groupName_;
	App* parent_ = nullptr;
	bool isGroup_ = false;
	std::vector<std::unique_ptr<Option>> options_;
	std::vector<std::unique_ptr<App>> subcommands_;
	std::vector<std::unique_ptr<App>> groups_;
	std::vector<Option*> excludedOptions_;
	std::vector<App*> excludedGroups_;
	Option* helpFlag_ = nullptr;
	bool requireSubcommand_ = false;
	std::size_t requireSubcommandMin_ = 0;
	std::size_t requireSubcommandMax_ = 0;
	std::size_t requireOptionMin_ = 0;
	std::size_t requireOptionMax_ = 0;
	std::size_t parsed_ = 0;
	std::vector<App*> parsedSubcommands_;
};

using Option_group = App;

} // namespace CLI

// NOLINTBEGIN
#define CLI11_PARSE(app, ...)                                                                      \
	try {                                                                                            \
		(app).parse(__VA_ARGS__);                                                                      \
	} catch (const CLI::ParseError& e) {                                                             \
		return (app).exit(e);                                                                          \
	}
// NOLINTEND
