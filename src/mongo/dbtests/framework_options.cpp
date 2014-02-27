/*
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/dbtests/framework_options.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/query/new_find.h"
#include "mongo/db/repl/replication_server_status.h"  // replSettings
#include "mongo/db/storage_options.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/password.h"

namespace mongo {

    FrameworkGlobalParams frameworkGlobalParams;

    Status addTestFrameworkOptions(moe::OptionSection* options) {

        options->addOptionChaining("help", "help", moe::Switch, "show this usage information");

        options->addOptionChaining("host", "host,h", moe::String,
                "mongo host to store performance results (Default: perfdb.10gen.cc)");

        options->addOptionChaining("port", "port", moe::Int,
                "server port. Can also use --host hostname:port")
                                  .validRange(0, 65535);

        options->addOptionChaining("dbpath", "dbpath", moe::String,
                "db data path for this test run. NOTE: the contents of this directory will "
                "be overwritten if it already exists")
                                  .setDefault(moe::Value(default_test_dbpath));

        options->addOptionChaining("debug", "debug", moe::Switch, "run tests with verbose output");

        options->addOptionChaining("list", "list,l", moe::Switch, "list available test suites");

        options->addOptionChaining("bigfiles", "bigfiles", moe::Switch,
                "use big datafiles instead of smallfiles which is the default");

        options->addOptionChaining("filter", "filter,f", moe::String,
                "string substring filter on test name");

        options->addOptionChaining("verbose", "verbose,v", moe::Switch, "verbose");

        options->addOptionChaining("dur", "dur", moe::Switch,
                "enable journaling (currently the default)");

        options->addOptionChaining("nodur", "nodur", moe::Switch, "disable journaling");

        options->addOptionChaining("seed", "seed", moe::UnsignedLongLong, "random number seed");

        options->addOptionChaining("runs", "runs", moe::Int, "number of times to run each test");

        options->addOptionChaining("perfHist", "perfHist", moe::Unsigned,
                "number of back runs of perf stats to display");


        options->addOptionChaining("suites", "suites", moe::StringVector, "test suites to run")
                                  .hidden()
                                  .positional(1, -1);

        options->addOptionChaining("nopreallocj", "nopreallocj", moe::Switch,
                "disable journal prealloc")
                                  .hidden();


        return Status::OK();
    }

    std::string getTestFrameworkHelp(const StringData& name, const moe::OptionSection& options) {
        StringBuilder sb;
        sb << "usage: " << name << " [options] [suite]...\n"
            << options.helpString() << "suite: run the specified test suite(s) only\n";
        return sb.str();
    }

    bool handlePreValidationTestFrameworkOptions(const moe::Environment& params,
                                                   const std::vector<std::string>& args) {
        if (params.count("help")) {
            std::cout << getTestFrameworkHelp(args[0], moe::startupOptions) << std::endl;
            return false;
        }

        if (params.count("list")) {
            std::vector<std::string> suiteNames = mongo::unittest::getAllSuiteNames();
            for ( std::vector<std::string>::const_iterator i = suiteNames.begin();
                    i != suiteNames.end(); ++i ) {

                std::cout << *i << std::endl;
            }
            return false;
        }

        return true;
    }

    Status storeTestFrameworkOptions(const moe::Environment& params,
                                     const std::vector<std::string>& args) {

        string host = "perfdb.10gen.cc";
        int port = 27017;
        if (params.count("host")) {
            host = params["host"].as<string>();
        }
        if (params.count("port")) {
            port = params["port"].as<int>();
        }
        frameworkGlobalParams.host = HostAndPort(host, port);

        if (params.count("dbpath")) {
            frameworkGlobalParams.dbpathSpec = params["dbpath"].as<string>();
        }

        if (params.count("seed")) {
            frameworkGlobalParams.seed = params["seed"].as<unsigned long long>();
        }

        if (params.count("runs")) {
            frameworkGlobalParams.runsPerTest = params["runs"].as<int>();
        }

        if (params.count("perfHist")) {
            frameworkGlobalParams.perfHist = params["perfHist"].as<unsigned>();
        }

        bool nodur = false;
        if( params.count("nodur") ) {
            nodur = true;
            storageGlobalParams.dur = false;
        }
        if( params.count("dur") || storageGlobalParams.dur ) {
            storageGlobalParams.dur = true;
        }

        if( params.count("nopreallocj") ) {
            storageGlobalParams.preallocj = false;
        }

        if (params.count("debug") || params.count("verbose") ) {
            logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(1));
        }

        boost::filesystem::path p(frameworkGlobalParams.dbpathSpec);

        /* remove the contents of the test directory if it exists. */
        try {
            if (boost::filesystem::exists(p)) {
                if (!boost::filesystem::is_directory(p)) {
                    StringBuilder sb;
                    sb << "ERROR: path \"" << p.string() << "\" is not a directory";
                    sb << getTestFrameworkHelp(args[0], moe::startupOptions);
                    return Status(ErrorCodes::BadValue, sb.str());
                }
                boost::filesystem::directory_iterator end_iter;
                for (boost::filesystem::directory_iterator dir_iter(p);
                        dir_iter != end_iter; ++dir_iter) {
                    boost::filesystem::remove_all(*dir_iter);
                }
            }
            else {
                boost::filesystem::create_directory(p);
            }
        }
        catch (const boost::filesystem::filesystem_error& e) {
            StringBuilder sb;
            sb << "boost::filesystem threw exception: " << e.what();
            return Status(ErrorCodes::BadValue, sb.str());
        }

        string dbpathString = p.string();
        storageGlobalParams.dbpath = dbpathString.c_str();

        storageGlobalParams.prealloc = false;

        // dbtest defaults to smallfiles
        storageGlobalParams.smallfiles = true;
        if( params.count("bigfiles") ) {
            storageGlobalParams.dur = true;
        }

        replSettings.oplogSize = 10 * 1024 * 1024;

        DEV log() << "_DEBUG build" << endl;
        if( sizeof(void*)==4 )
            log() << "32bit" << endl;
        log() << "random seed: " << frameworkGlobalParams.seed << endl;

        if( time(0) % 3 == 0 && !nodur ) {
            if (!storageGlobalParams.dur) {
                storageGlobalParams.dur = true;
                log() << "****************" << endl;
                log() << "running with journaling enabled to test that. dbtests will do this "
                      << "occasionally even if --dur is not specified." << endl;
                log() << "****************" << endl;
            }
        }

        if (params.count("suites")) {
            frameworkGlobalParams.suites = params["suites"].as< vector<string> >();
        }

        frameworkGlobalParams.filter = "";
        if ( params.count( "filter" ) ) {
            frameworkGlobalParams.filter = params["filter"].as<string>();
        }

        if (debug && storageGlobalParams.dur) {
            log() << "_DEBUG: automatically enabling storageGlobalParams.durOptions=8 "
                  << "(DurParanoid)" << endl;
            // this was commented out.  why too slow or something?
            storageGlobalParams.durOptions |= 8;
        }

        return Status::OK();
    }

}
