#pragma once

#include "Blueprint/Workspace/Configuration.hpp"

namespace blueprint { class File; }

namespace blueprint
{
    class Project
    {
    public:
        void SetName(const std::string& name);
        const std::string& GetName() const;

        void SetFile(const filesystem::path& file);
        const filesystem::path& GetFile() const;

    public:
        using Configurations = std::vector<std::unique_ptr<Configuration>>;

        void AddConfiguration(std::unique_ptr<Configuration> configuration);
        const Configurations& GetConfigurations() const;

    public:
        using Files = std::vector<std::shared_ptr<File>>;

        void AddFile(std::shared_ptr<File> file);
        const Files& GetFiles() const;

    private:
        std::string name_;
        filesystem::path file_;

        Configurations configurations_;
        Files files_;
    };
}
