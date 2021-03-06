#include "Blueprint/Parser/Parser.hpp"

#include "Blueprint/Database/Database.hpp"
#include "Blueprint/Database/SqliteApi.hpp"
#include "BlueprintClang/Index.hpp"
#include "Blueprint/Parser/Visitors/NamespaceVisitor.hpp"
#include "Blueprint/Parser/CommandLineArguments.hpp"
#include "BlueprintReflection/Registry/TypeRegistry.hpp"
#include "BlueprintReflection/Type/ClassType.hpp"
#include "BlueprintReflection/Type/EnumType.hpp"
#include "BlueprintReflection/Visitor/TypeEnumerator.hpp"
#include "Blueprint/Utilities/FileSystem.hpp"
#include "Blueprint/Utilities/JsonImporter.hpp"
#include "Blueprint/Utilities/ScopeTimer.hpp"
#include "Blueprint/Utilities/WorkingDirectory.hpp"
#include "Blueprint/Workspace/File.hpp"
#include "Blueprint/Workspace/FileManager.hpp"
#include "Blueprint/Workspace/Workspace.hpp"

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

        void EnsureDirectoryExists(const filesystem::path& directory)
        {
            if (!directory.is_directory())
            {
                filesystem::create_directory(directory);
            }
        }

        std::string GetFilenameWithoutExtension(const filesystem::path& filePath)
        {
            auto extension = filePath.extension();
            auto filename = filePath.filename();

            return !extension.empty() ? filename.substr(0, filename.length() - extension.length() - 1) : filename;
        }

        void SaveTypes(const reflection::TypeEnumerator& enumerator, database::Database& database)
        {
            std::cout << std::endl;
            std::cout << "> saving database :" << std::endl;

            ScopeTimer timer([](double time) {
                std::cout << time << "s" << std::endl;
            });

            auto& classes = enumerator.GetClasses();
            auto& enums = enumerator.GetEnums();

            std::vector<const reflection::Type*> types;
            types.reserve(classes.size() + enums.size());

            types.insert(types.end(), classes.begin(), classes.end());
            types.insert(types.end(), enums.begin(), enums.end());

            database.InsertTypes(types);
        }

        void ListTypes(const reflection::TypeEnumerator& enumerator)
        {
            std::cout << std::endl;
            std::cout << "> listing types :" << std::endl;

            ScopeTimer timer([](double time) {
                std::cout << "> " << time << "s" << std::endl;
            });

            auto sorter = [](auto lhs, auto rhs)
            {
                return lhs->GetFullName() < rhs->GetFullName();
            };

            auto classes = enumerator.GetClasses();
            auto enums = enumerator.GetEnums();

            if (!classes.empty())
            {
                std::cout << ">> classes : (" << classes.size() << ")" << std::endl;

                std::sort(classes.begin(), classes.end(), sorter);

                for (auto type : classes)
                {
                    std::cout << ">>> " << type->GetFullName() << std::endl;
                }
            }

            if (!enums.empty())
            {
                std::cout << ">> enum : (" << enums.size() << ")" << std::endl;

                std::sort(enums.begin(), enums.end(), sorter);

                for (auto type : enums)
                {
                    std::cout << ">>> " << type->GetFullName() << std::endl;
                }
            }
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

        FileManager& GetFileManager()
        {
            return fileManager_;
        }

        database::Database& GetDatabase()
        {
            return database_;
        }

    private:
        clang::Index index_;

        reflection::TypeRegistry typeRegistry_;

        FileSystem fileSystem_;
        FileManager fileManager_{fileSystem_};

        database::Database database_;
    };

    struct Parser::FileContext
    {
        const Configuration* config{nullptr};

        filesystem::path filePath;

        CommandLineArguments arguments;

        bool isSource{false};
        bool isHeader{false};
        bool includePrecompiled{false};
        bool saveDependencies{true};
        bool saveArguments{true};
        bool save{false};
    };

    Parser::Parser()
        : pimpl_(std::make_unique<Impl>())
    {}

    Parser::~Parser() = default;

    bool Parser::ParseWorkspace(const filesystem::path& filePath)
    {
        if (filePath.str().empty())
        {
            std::cout << "invalid workspace filename" << std::endl;
            return false;
        }

        WorkingDirectory::SetCurrent(filePath.make_absolute().parent_path().str());
        std::cout << "{ cwd : " << WorkingDirectory::GetCurrent() << " }" << std::endl;

        internal::EnsureDirectoryExists(outputDirectory_);

        auto workspace = JsonImporter::ImportWorkspace(pimpl_->GetFileManager(), filePath.filename());
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

            auto originalOutput = outputDirectory_;

            for (auto& project : workspace->GetProjects())
            {
                outputDirectory_ = originalOutput / project->GetName();
                internal::EnsureDirectoryExists(outputDirectory_);

                if (!ParseProject(project.get()))
                {
                    return false;
                }
            }

            outputDirectory_ = originalOutput;
        }

        reflection::TypeEnumerator enumerator;
        pimpl_->GetTypeRegistry().Accept(enumerator);

        if (listTypes_)
        {
            internal::ListTypes(enumerator);
        }

        pimpl_->GetDatabase().Initialize(outputDirectory_ / "registry.db");
        internal::SaveTypes(enumerator, pimpl_->GetDatabase());

        return true;
    }

    bool Parser::ParseProject(const filesystem::path& filePath)
    {
        if (filePath.str().empty())
        {
            std::cout << "invalid project filename" << std::endl;
            return false;
        }

        auto project = JsonImporter::ImportProject(pimpl_->GetFileManager(), filePath);
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

        if (!project->GetConfigurations().empty())
        {
            config = project->GetConfigurations()[0].get();
        }

        if (config == nullptr)
        {
            std::cout << "invalid config" << std::endl;
            return false;
        }

        CommandLineArguments arguments;
        arguments.ImportConfig(config);
        arguments.Add("-std=c++14"); // Language standard to compile for
        arguments.Add("-w");         // Suppress all warnings

    #if defined(_MSC_VER)
        arguments.Add("-fms-compatibility-version=19");
    #endif

        if (verbose_)
        {
            arguments.Add("-v"); // Show commands to run and use verbose output
        }

        {
            ScopeTimer timer([](double time) {
                std::cout << ">> " << time << "s" << std::endl;
            });

            if (config->HasPrecompiledHeader())
            {
                FileContext context;

                context.config = config;
                context.arguments = arguments;
                context.filePath = config->GetPrecompiledSource();
                context.save = true;

                if (!ParseSourceFile(context))
                {
                    return false;
                }
            }

            for (auto& file : project->GetFiles())
            {
                if (file.get() && file->IsHeader())
                {
                    if (config->HasPrecompiledHeader() && config->GetPrecompiledSource() == file->GetFile().str())
                    {
                        continue;
                    }

                    FileContext context;

                    context.config = config;
                    context.arguments = arguments;
                    context.filePath = file->GetFile();
                    context.isSource = file->IsSource();
                    context.isHeader = file->IsHeader();
                    context.includePrecompiled = true;

                    if (!ParseSourceFile(context))
                    {
                        return false;
                    }
                }
            }
        }

        return true;
    }

    bool Parser::ParseSourceFile(FileContext& context)
    {
        if (context.config->HasPrecompiledHeader() && context.config->GetPrecompiledSource() == context.filePath.str())
        {
            std::cout << ">>> pch     : " << context.filePath << std::endl;
        }
        else if (context.isSource)
        {
            std::cout << ">>> source  : " << context.filePath << std::endl;
        }
        else if (context.isHeader)
        {
            std::cout << ">>> header  : " << context.filePath << std::endl;
        }

        IncludePrecompiled(context);
        SaveDependencies(context);
        SaveArguments(context);

        clang::TranslationUnit translationUnit;

        unsigned options = CXTranslationUnit_SkipFunctionBodies;

        if (context.save)
        {
            options |= CXTranslationUnit_ForSerialization;
            options |= CXTranslationUnit_Incomplete;
        }

        auto result = pimpl_->GetIndex().ParseSourceFile(context.filePath.str(), context.arguments.GetArguments(), options, translationUnit);

        internal::DisplayDiagnostics(translationUnit);

        if (result == CXError_Success)
        {
            clang::NamespaceVisitor visitor(pimpl_->GetTypeRegistry());
            visitor.Visit(translationUnit.GetCursor());

            if (context.save)
            {
                auto saveFile = internal::GetFilenameWithoutExtension(context.filePath) + ".pch";
                auto savePath = !outputDirectory_.str().empty() ? outputDirectory_ / saveFile : saveFile;

                translationUnit.Save(savePath.str());
            }
        }
        else
        {
            auto errorString = [](auto errorCode)
            {
                switch (errorCode)
                {
                    case CXError_Success: return "success";
                    case CXError_Failure: return "failure";
                    case CXError_Crashed: return "crashed";
                    case CXError_InvalidArguments: return "invalid arguments";
                    case CXError_ASTReadError: return "ast read error";
                    default: return "unknow error code";
                }
            };

            std::cout << std::endl;
            std::cout << "failed to create TranslationUnit (" << errorString(result) << ")" << std::endl;
            std::cout << std::endl;

            return false;
        }

        return true;
    }

    void Parser::IncludePrecompiled(FileContext& context) const
    {
        if (context.includePrecompiled && context.config->HasPrecompiledHeader())
        {
            auto pchFile = internal::GetFilenameWithoutExtension(context.config->GetPrecompiledSource()) + ".pch";
            auto pchPath = !outputDirectory_.str().empty() ? (outputDirectory_ / pchFile).str() : pchFile;

            context.arguments.Add("-include-pch");
            context.arguments.Add(pchPath);
        }
    }

    void Parser::SaveDependencies(FileContext& context) const
    {
        if (context.saveDependencies)
        {
            auto depFile = internal::GetFilenameWithoutExtension(context.filePath) + ".dep";
            auto depPath = !outputDirectory_.str().empty() ? (outputDirectory_ / depFile).str() : depFile;

            context.arguments.Add("-MMD");
            context.arguments.Add("-MF");
            context.arguments.Add(depPath);
        }
    }

    void Parser::SaveArguments(FileContext& context) const
    {
        if (context.saveArguments)
        {
            auto argFile = internal::GetFilenameWithoutExtension(context.filePath) + ".txt";
            auto argPath = !outputDirectory_.str().empty() ? outputDirectory_ / argFile : argFile;

            context.arguments.Save(argPath);
        }
    }
}
