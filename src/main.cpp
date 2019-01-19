/**
 * @file main.cpp
 * @brief
 * @author Nathan Zhou
 * @date 2019-01-13
 * @bug No known bugs.
 */

#include <iostream>
#include <boost/filesystem.hpp>
#include <abc_plus.h>

using namespace boost::filesystem;
using namespace abc_plus;

void PrepoBenchtoAigBlif(const path &bench_dir, const path &blif_dir, const std::vector<std::string> &files);

int main(int argc, char *argv[]) {
    path project_source_dir(PROJECT_SOURCE_DIR);
    path bench_dir = project_source_dir / "benchmark" / "bench";
    path blif_dir = project_source_dir / "benchmark" / "blif";
    std::vector<std::string> iscas_85 = {"c17", "c432", "c499", "c880", "c1355", "c1908", "c2670", "c3540", "c5315", "c6288", "c7552"};

    PrepoBenchtoAigBlif(bench_dir, blif_dir, iscas_85);

    path blif_file = blif_dir / "c17.blif";
    auto framework = Framework::GetFramework();
    framework->ReadBlif(blif_file.string());
    auto ntk = framework->GetNtk();

    return 0;
}

void PrepoBenchtoAigBlif(const path &bench_dir, const path &blif_dir, const std::vector<std::string> &files) {
    auto framework = Framework::GetFramework();
    for (const auto &file : files) {
        path bench_file = bench_dir / file;
        bench_file.replace_extension("bench");
        path blif_file = blif_dir / file;
        blif_file.replace_extension("blif");

        framework->ReadBench(bench_file.string());
        framework->CmdExec("strash");
        framework->WriteBlif(blif_file.string());
    }
}
