#include "Blueprint/Parser/Parser.hpp"

#if defined(EXTERN_CLANG_ENABLED)

#include "Blueprint/Parser/Clang/Index.hpp"
#include "Blueprint/Parser/Visitors/NamespaceVisitor.hpp"
#include "Blueprint/Parser/CommandLineArguments.hpp"
#include "Blueprint/Reflection/TypeRegistry.hpp"
#include "Blueprint/Utilities/ScopeTimer.hpp"
#include "Blueprint/Utilities/WorkingDirectory.hpp"
#include "Blueprint/Workspace/JsonImporter.hpp"
#include "Blueprint/Workspace/Workspace.hpp"

#include <iostream>

namespace blueprint
{
    namespace internal
    {
        void DisplayDiagnostics(const clang::TranslationUnit& translationUnit)
        {
            static auto pragmaOnceInMainFile = "warning: #pragma once in main file";
            static auto options = clang::Diagnostic::GetDefaultOptions();

            for (auto& diagnostic : translationUnit.GetDiagnostics())
            {
                auto message = diagnostic.Format(options).Get();

                if (message.find(pragmaOnceInMainFile) != std::string::npos)
                {
                    continue;
                }

                std::cout << message << std::endl;
            }
        }

        bool IsSourceFile(const filesystem::path& file)
        {
            auto extension = file.extension();

            return extension == "c"
                || extension == "cc"
                || extension == "cpp";
        }

        bool IsHeaderFile(const filesystem::path& file)
        {
            auto extension = file.extension();

            return extension == "h"
                || extension == "hh"
                || extension == "hpp";
        }
    }

    class Parser::Impl
    {
    public:
        const clang::Index& GetIndex() const
        {
            return index_;
        }

        reflection::TypeRegistry& GetTypeRegistry()
        {
            return typeRegistry_;
        }

    private:
        clang::Index index_;

        reflection::TypeRegistry typeRegistry_;
    };

    Parser::Parser()
        : pimpl_(new Impl())
    {}

    Parser::~Parser() = default;

    bool Parser::ParseWorkspace(const filesystem::path& filePath)
    {
        WorkingDirectory::SetCurrent(filePath.make_absolute().parent_path().str());
        std::cout << "{ cwd : " << WorkingDirectory::GetCurrent() << " }" << std::endl;

        auto workspace = JsonImporter::ImportWorkspace(filePath.filename());
        return ParseWorkspace(workspace.get());
    }

    bool Parser::ParseWorkspace(const Workspace* workspace)
    {
        if (workspace == nullptr)
        {
            std::cout << "invalid workspace" << std::endl;
            return false;
        }

        std::cout << "{ libclang index version " << CINDEX_VERSION_STRING << " }" << std::endl;
        std::cout << "{ libclang " << clang::String(clang_getClangVersion()).Get() << " }" << std::endl;
        std::cout << std::endl;

        std::cout << "> workspace : " << workspace->GetName() << std::endl;

        {
            ScopeTimer timer([](double time) {
                std::cout << "> " << time << "s" << std::endl;
            });

            for (auto& project : workspace->GetProjects())
            {
                ParseProject(project.get());
            }
        }

        return true;
    }

    bool Parser::ParseProject(const filesystem::path& filePath)
    {
        auto project = JsonImporter::ImportProject(filePath);
        return ParseProject(project.get());
    }

    bool Parser::ParseProject(const Project* project)
    {
        if (project == nullptr)
        {
            std::cout << "invalid project" << std::endl;
            return false;
        }

        std::cout << ">> project  : " << project->GetName() << std::endl;

        const Configuration* config = nullptr;

        if (project->GetConfigurations().size() > 0)
        {
            config = project->GetConfigurations()[0].get();
        }

        {
            ScopeTimer timer([](double time) {
                std::cout << ">> " << time << "s" << std::endl;
            });

            for (auto& source : project->GetSources())
            {
                if (internal::IsHeaderFile(source))
                {
                    ParseSourceFile(source, config);
                }
            }
        }

        return true;
    }

    bool Parser::ParseSourceFile(const filesystem::path& filePath, const Configuration* config)
    {
        std::cout << ">>> header  : " << filePath << std::endl;

        unsigned options = CXTranslationUnit_SkipFunctionBodies;

        CommandLineArguments arguments;
        arguments.Add("-std=c++14");
        arguments.ImportConfig(config);

        if (verbose_)
        {
            arguments.Add("-v");
        }

        clang::TranslationUnit translationUnit;

        {
            /*ScopeTimer parseTimer([](double time){
                std::cout << " (parse: " << time << "s)" << std::endl;
            });*/

            translationUnit = pimpl_->GetIndex().ParseSourceFile(filePath.str(), arguments, options);

            internal::DisplayDiagnostics(translationUnit);
        }

        if (translationUnit)
        {
            /*ScopeTimer timer([](double time){
                std::cout << " (visit: " << time << "s)";
            });*/

            NamespaceVisitor visitor(pimpl_->GetTypeRegistry());
            visitor.Visit(translationUnit.GetCursor());
        }

        return true;
    }
}

#endif
