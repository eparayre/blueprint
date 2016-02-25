#include "Probe/Project.hpp"

namespace probe
{
    void Project::SetName(const std::string& name)
    {
        name_ = name;
    }

    const std::string& Project::GetName() const
    {
        return name_;
    }

    void Project::AddFile(const std::string& file)
    {
        files_.push_back(file);
    }

    const Project::StringArray& Project::GetFiles() const
    {
        return files_;
    }
}
