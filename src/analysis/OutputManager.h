#pragma once

#include <list>
#include <memory>
#include <string>

#include "base/WrapTFile.h"

class TFile;
class TDirectory;

namespace ant {
namespace output {

class OutputManager {
protected:

    using file_list_t = std::list< std::unique_ptr< WrapTFile > >;
    file_list_t files;
    TDirectory* current_dir = nullptr;

public:
    OutputManager();

    void SetNewOutput(const std::string& filename);

    TDirectory* CurrentDirectory() { return current_dir; }

    OutputManager(const OutputManager&) = delete;
    OutputManager& operator= (const OutputManager&) = delete;
};

}
}

