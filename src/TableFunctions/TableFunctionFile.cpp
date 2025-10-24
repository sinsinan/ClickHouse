#include <Interpreters/parseColumnsListForTableFunction.h>
#include <TableFunctions/ITableFunctionFileLike.h>
#include <TableFunctions/TableFunctionFile.h>

#include <TableFunctions/registerTableFunctions.h>
#include <Access/Common/AccessFlags.h>
#include <Core/Settings.h>
#include <Interpreters/Context.h>
#include <Storages/ColumnsDescription.h>
#include <Storages/StorageFile.h>
#include <TableFunctions/TableFunctionFactory.h>
#include <Interpreters/evaluateConstantExpression.h>
#include <Formats/FormatFactory.h>
#include <Storages/HivePartitioningUtils.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTLiteral.h>
#include "base/types.h"


namespace DB
{
namespace Setting
{
    extern const SettingsBool allow_archive_path_syntax;
    extern const SettingsString rename_files_after_processing;
}

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int LOGICAL_ERROR;
}

void TableFunctionFile::parseFirstArguments(const ASTPtr & arg, const ContextPtr & context)
{
    if (context->getApplicationType() != Context::ApplicationType::LOCAL)
    {
        ITableFunctionFileLike::parseFirstArguments(arg, context);
        StorageFile::parseFileSources(std::move(filenames), filenames, path_to_archives, context->getSettingsRef()[Setting::allow_archive_path_syntax]);
        return;
    }

    const auto * literal = arg->as<ASTLiteral>();
    if (!literal)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "First argument must be a literal");

    auto type = literal->value.getType();
    if (type == Field::Types::String)
    {
        String path = literal->value.safeGet<String>();
        if (path == "stdin" || path == "-") { fd = STDIN_FILENO; return; }
        if (path == "stdout") { fd = STDOUT_FILENO; return; }
        if (path == "stderr") { fd = STDERR_FILENO; return; }

        String single_filename_parsed;
        String path_to_archive;
        StorageFile::parseFileSource(std::move(path), single_filename_parsed, path_to_archive, context->getSettingsRef()[Setting::allow_archive_path_syntax]);
        filenames.push_back(single_filename_parsed);
        path_to_archives.push_back(path_to_archive);
    }
    else if (type == Field::Types::Int64 || type == Field::Types::UInt64)
    {
        fd = static_cast<int>(
            (type == Field::Types::Int64) ? literal->value.safeGet<Int64>() : literal->value.safeGet<UInt64>());
        if (fd < 0)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "File descriptor must be non-negative");
    }
    else
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "The first argument of table function '{}' must be a path or a file descriptor", getName());
}

std::optional<String> TableFunctionFile::tryGetFormatFromFirstArgument()
{
    if (fd >= 0)
        return FormatFactory::instance().tryGetFormatFromFileDescriptor(fd);
    if (!filenames.empty())
        return FormatFactory::instance().tryGetFormatFromFileName(filenames.front());
    return {};
}

StoragePtr TableFunctionFile::getStorage(
    const Strings & sources,
    const String & format_,
    const ColumnsDescription & columns,
    ContextPtr global_context,
    const std::string & table_name,
    const std::string & compression_method_,
    bool /*is_insert_query*/) const
{
    // For `file` table function, we are going to use format settings from the
    // query context.
    StorageFile::CommonArguments args{
        WithContext(global_context),
        StorageID(getDatabaseName(), table_name),
        format_,
        std::nullopt /*format settings*/,
        compression_method_,
        columns,
        ConstraintsDescription{},
        String{},
        global_context->getSettingsRef()[Setting::rename_files_after_processing],
    };

    if (fd >= 0)
        return std::make_shared<StorageFile>(fd, args);

    return std::make_shared<StorageFile>(sources, global_context->getUserFilesPath(), false, args);
}

ColumnsDescription TableFunctionFile::getActualTableStructure(ContextPtr context, bool /*is_insert_query*/) const
{
    if (structure == "auto")
    {
        if (fd >= 0)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Schema inference is not supported for table function '{}' with file descriptor", getName());

        if (filenames.empty())
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Cannot infer structure without a file path for table function '{}'", getName());

        size_t total_bytes_to_read = 0;

        Strings paths;
        std::optional<std::vector<StorageFile::FileArchiveInfo>> archive_info;
        if (path_to_archives.empty())
        {
            for (const auto & file : filenames)
            {
                auto new_paths = StorageFile::getPathsList(file, context->getUserFilesPath(), context, total_bytes_to_read);
                paths.insert(paths.end(), new_paths.begin(), new_paths.end());
            }
        }
        else
        {
            if (filenames.size() > 1)
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Archive path is set, but multiple files are provided");
            archive_info
                = StorageFile::getArchiveInfos(path_to_archives, filenames, context->getUserFilesPath(), context, total_bytes_to_read);
        }


        ColumnsDescription columns;
        if (format == "auto")
            columns = StorageFile::getTableStructureAndFormatFromFile(paths, compression_method, std::nullopt, context, archive_info).first;
        else
            columns = StorageFile::getTableStructureFromFile(format, paths, compression_method, std::nullopt, context, archive_info);

        auto sample_path = paths.empty() ? String{} : paths.front();

        HivePartitioningUtils::setupHivePartitioningForFileURLLikeStorage(
            columns,
            sample_path,
            /* inferred_schema */ true,
            /* format_settings */ std::nullopt,
            context);

        return columns;
    }

    return parseColumnsListFromString(structure, context);
}

void registerTableFunctionFile(TableFunctionFactory & factory)
{
    factory.registerFunction<TableFunctionFile>();
}

}