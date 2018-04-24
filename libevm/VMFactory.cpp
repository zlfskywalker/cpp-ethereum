/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "VMFactory.h"
#include "EVMC.h"
#include "LegacyVM.h"
#include "interpreter.h"

#include <boost/dll.hpp>
#include <boost/filesystem.hpp>

#if ETH_EVMJIT
#include <evmjit.h>
#endif

#if ETH_HERA
#include <hera.h>
#endif

namespace dll = boost::dll;
namespace fs = boost::filesystem;
namespace po = boost::program_options;

namespace dev
{
namespace eth
{
namespace
{
using evmc_create_fn = evmc_instance*();
static_assert(std::is_same<decltype(evmc_create_interpreter), evmc_create_fn>::value, "");

auto g_kind = VMKind::Legacy;

struct VMMapEntry
{
    VMKind kind;
    std::function<evmc_create_fn> createFn;
};

std::map<std::string, VMMapEntry> g_vmMap{
    {"interpreter", {VMKind::Interpreter, {}}},
    {"legacy", {VMKind::Legacy, {}}},
#if ETH_EVMJIT
    {"jit", {VMKind::JIT, {}}},
#endif
#if ETH_HERA
    {"hera", {VMKind::Hera, {}}},
#endif
};
}

void validate(boost::any& v, const std::vector<std::string>& values, VMKind* /* target_type */, int)
{
    // Make sure no previous assignment to 'v' was made.
    po::validators::check_first_occurrence(v);

    // Extract the first string from 'values'. If there is more than
    // one string, it's an error, and exception will be thrown.
    const std::string& s = po::validators::get_single_string(values);

    if (g_vmMap.count(s) == 0)
        throw po::validation_error(po::validation_error::invalid_option_value);
}

namespace
{
/// The name of the program option --evmc. The boost will trim the tailing
/// space and we can reuse this variable in exception message.
const char c_evmcPrefix[] = "evmc ";

/// The list of EVM-C options stored as pairs of (name, value).
std::vector<std::pair<std::string, std::string>> s_evmcOptions;

/// The additional parser for EVM-C options. The options should look like
/// `--evmc name=value` or `--evmc=name=value`. The boost pass the strings
/// of `name=value` here. This function splits the name and value or reports
/// the syntax error if the `=` character is missing.
void parseEvmcOptions(const std::vector<std::string>& _opts)
{
    for (auto& s : _opts)
    {
        auto separatorPos = s.find('=');
        if (separatorPos == s.npos)
            throw po::invalid_syntax{po::invalid_syntax::missing_parameter, c_evmcPrefix + s};
        auto name = s.substr(0, separatorPos);
        auto value = s.substr(separatorPos + 1);
        s_evmcOptions.emplace_back(std::move(name), std::move(value));
    }
}

void loadEvmcDlls(const std::vector<std::string>& _paths)
{
    for (auto& path : _paths)
    {
        auto symbols = dll::library_info{path}.symbols();
        auto it = std::find_if(symbols.begin(), symbols.end(),
            [](const std::string& symbol) { return symbol.find("evmc_create_") == 0; });
        if (it == symbols.end())
        {
            // This is what boost is doing when symbol not found.
            auto ec = std::make_error_code(std::errc::invalid_seek);
            std::string what = "loading " + path + " failed: EVMC create function not found";
            BOOST_THROW_EXCEPTION(std::system_error(ec, what));
        }

        auto createFn = dll::import<evmc_create_fn>(path, *it);
        evmc_instance* vm = createFn();
        std::string name = vm->name;
        std::cout << "Loaded EVM " << name << " " << vm->version << "\n";
        vm->destroy(vm);

        // It will overwrite existing entries.
        g_vmMap[name] = {VMKind::DLL, std::move(createFn)};
    }
}
}

std::vector<std::pair<std::string, std::string>>& evmcOptions() noexcept
{
    return s_evmcOptions;
};

po::options_description vmProgramOptions(unsigned _lineLength)
{
    // It must be a static object because boost expects const char*.
    static const std::string description = [] {
        std::string names;
        for (auto& entry : g_vmMap)
        {
            if (!names.empty())
                names += ", ";
            names += entry.first;
        }

        return "Select VM implementation. Available options are: " + names + ".";
    }();

    po::options_description opts("VM Options", _lineLength);
    auto add = opts.add_options();

    add("vm",
        po::value<VMKind>()
            ->value_name("<name>")
            ->default_value(VMKind::Legacy, "legacy")
            ->notifier(VMFactory::setKind),
        description.data());

    add(c_evmcPrefix,
        po::value<std::vector<std::string>>()
            ->value_name("<option>=<value>")
            ->notifier(parseEvmcOptions),
        "EVMC option");

    add("evmc-load",
        po::value<std::vector<std::string>>()->value_name("<path>")->notifier(loadEvmcDlls),
        "Path to EVMC dynamic loaded VM");

    return opts;
}


void VMFactory::setKind(VMKind _kind)
{
    g_kind = _kind;
}

std::unique_ptr<VMFace> VMFactory::create()
{
    return create(g_kind);
}

std::unique_ptr<VMFace> VMFactory::create(VMKind _kind)
{
    switch (_kind)
    {
#ifdef ETH_EVMJIT
    case VMKind::JIT:
        return std::unique_ptr<VMFace>(new EVMC{evmjit_create()});
#endif
#ifdef ETH_HERA
    case VMKind::Hera:
        return std::unique_ptr<VMFace>(new EVMC{evmc_create_hera()});
#endif
    case VMKind::Interpreter:
        return std::unique_ptr<VMFace>(new EVMC{evmc_create_interpreter()});
    case VMKind::Legacy:
    default:
        return std::unique_ptr<VMFace>(new LegacyVM);
    }
}
}
}
