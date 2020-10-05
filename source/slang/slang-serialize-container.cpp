// slang-serialize-container.cpp
#include "slang-serialize-container.h"

#include "../core/slang-text-io.h"
#include "../core/slang-byte-encode-util.h"

#include "../core/slang-math.h"

#include "slang-compiler.h"
#include "slang-serialize-ast.h"
#include "slang-serialize-ir.h"
#include "slang-serialize-source-loc.h"

namespace Slang {

/* static */SlangResult SerialContainerUtil::requestToData(EndToEndCompileRequest* request, const WriteOptions& options, SerialContainerData& out)
{
    SLANG_UNUSED(options);

    out.clear();

    auto linkage = request->getLinkage();
    auto sink = request->getSink();
    auto frontEndReq = request->getFrontEndReq();

    for (TranslationUnitRequest* translationUnit : frontEndReq->translationUnits)
    {
        auto module = translationUnit->module;
        auto irModule = module->getIRModule();

        // Root AST node
        auto moduleDecl = translationUnit->getModuleDecl();

        SLANG_ASSERT(irModule || moduleDecl);

        SerialContainerData::Module dstModule;

        // NOTE: The astBuilder is not set here, as not needed to be scoped for serialization (it is assumed the
        // TranslationUnitRequest stays in scope)

        dstModule.astRootNode = moduleDecl;
        dstModule.irModule = irModule;

        out.modules.add(dstModule);
    }

    auto program = request->getSpecializedGlobalAndEntryPointsComponentType();

    // Add all the target modules
    {
        
        for (auto target : linkage->targets)
        {
            auto targetProgram = program->getTargetProgram(target);
            auto irModule = targetProgram->getOrCreateIRModuleForLayout(sink);

            SerialContainerData::TargetComponent targetComponent;

            targetComponent.irModule = irModule;

            auto& dstTarget = targetComponent.target;

            dstTarget.floatingPointMode = target->floatingPointMode;
            dstTarget.profile = target->targetProfile;
            dstTarget.flags = target->targetFlags;
            dstTarget.codeGenTarget = target->target;

            out.targetComponents.add(targetComponent);
        }
    }

    // Entry points
    {
        auto entryPointCount = program->getEntryPointCount();
        for (Index ii = 0; ii < entryPointCount; ++ii)
        {
            auto entryPoint = program->getEntryPoint(ii);
            auto entryPointMangledName = program->getEntryPointMangledName(ii);

            SerialContainerData::EntryPoint dstEntryPoint;

            dstEntryPoint.name = entryPoint->getName();
            dstEntryPoint.mangledName = entryPointMangledName;
            dstEntryPoint.profile = entryPoint->getProfile();

            out.entryPoints.add(dstEntryPoint);
        }
    }

    return SLANG_OK;
}

/* static */SlangResult SerialContainerUtil::write(const SerialContainerData& data, const WriteOptions& options, RiffContainer* container)
{
    RefPtr<SerialSourceLocWriter> sourceLocWriter;

    // The string pool used across the whole of the container
    StringSlicePool containerStringPool(StringSlicePool::Style::Default);

    RiffContainer::ScopeChunk scopeModule(container, RiffContainer::Chunk::Kind::List, SerialBinary::kContainerFourCc);

    // Write the header
    {
        SerialBinary::ContainerHeader containerHeader;
        // Save the compression type if used - as can only serialize with a single compression type
        containerHeader.compressionType = uint32_t(options.compressionType);

        RiffContainer::ScopeChunk scopeHeader(container, RiffContainer::Chunk::Kind::Data, SerialBinary::kContainerHeaderFourCc);
        container->write(&containerHeader, sizeof(containerHeader));
    }

    if (data.modules.getCount() && (options.optionFlags & (SerialOptionFlag::IRModule | SerialOptionFlag::ASTModule)))
    {
        // Module list
        RiffContainer::ScopeChunk moduleListScope(container, RiffContainer::Chunk::Kind::List, SerialBinary::kModuleListFourCc);

        if (options.optionFlags & SerialOptionFlag::DebugInfo)
        {
            sourceLocWriter = new SerialSourceLocWriter(options.sourceManager);
        }

        RefPtr<SerialClasses> serialClasses;

        for (const auto& module : data.modules)
        {
            // Okay, we need to serialize this module to our container file.
            // We currently don't serialize it's name..., but support for that could be added.

            // Write the IR information
            if ((options.optionFlags & SerialOptionFlag::IRModule) && module.irModule)
            {
                IRSerialData serialData;
                IRSerialWriter writer;
                SLANG_RETURN_ON_FAIL(writer.write(module.irModule, sourceLocWriter, options.optionFlags, &serialData));
                SLANG_RETURN_ON_FAIL(IRSerialWriter::writeContainer(serialData, options.compressionType, container));
            }

            // Write the AST information

            if (options.optionFlags & SerialOptionFlag::ASTModule)
            {
                if (ModuleDecl* moduleDecl = as<ModuleDecl>(module.astRootNode))
                {
                    if (!serialClasses)
                    {
                        SLANG_RETURN_ON_FAIL(SerialClasses::create(serialClasses));
                    }

                    ModuleSerialFilter filter(moduleDecl);
                    SerialWriter writer(serialClasses, &filter);

                    writer.getExtraObjects().set(sourceLocWriter);
                    
                    // Add the module and everything that isn't filtered out in the filter.
                    writer.addPointer(moduleDecl);

                    // We can now serialize it into the riff container.
                    SLANG_RETURN_ON_FAIL(writer.writeIntoContainer(ASTSerialBinary::kSlangASTModuleFourCC, container));
                }
            }
        }

        if (data.targetComponents.getCount() && (options.optionFlags & SerialOptionFlag::IRModule))
        {
            // TODO: in the case where we have specialization, we might need
            // to serialize IR related to `program`...

            for (const auto& targetComponent : data.targetComponents)
            {
                IRModule* irModule = targetComponent.irModule;

                // Okay, we need to serialize this target program and its IR too...
                IRSerialData serialData;
                IRSerialWriter writer;

                SLANG_RETURN_ON_FAIL(writer.write(irModule, sourceLocWriter, options.optionFlags, &serialData));
                SLANG_RETURN_ON_FAIL(IRSerialWriter::writeContainer(serialData, options.compressionType, container));
            }
        }
    }

    if (data.entryPoints.getCount())
    {
        for (const auto& entryPoint : data.entryPoints)
        {
            RiffContainer::ScopeChunk entryPointScope(container, RiffContainer::Chunk::Kind::Data, SerialBinary::kEntryPointFourCc);

            SerialContainerBinary::EntryPoint dst;

            dst.name = uint32_t(containerStringPool.add(entryPoint.name->text));
            dst.profile = entryPoint.profile.raw;
            dst.mangledName = uint32_t(containerStringPool.add(entryPoint.mangledName));

            container->write(&dst, sizeof(dst));
        }
    }

    // We can now output the debug information. This is for all IR and AST 
    if (sourceLocWriter)
    {
        // Write out the debug info
        SerialSourceLocData debugData;
        sourceLocWriter->write(&debugData);

        debugData.writeContainer(options.compressionType, container);
    }

    // Write the container string table
    if (containerStringPool.getAdded().getCount() > 0)
    {
        RiffContainer::ScopeChunk stringTableScope(container, RiffContainer::Chunk::Kind::Data, SerialBinary::kStringTableFourCc);

        List<char> encodedTable;
        SerialStringTableUtil::encodeStringTable(containerStringPool, encodedTable);
        
        container->write(encodedTable.getBuffer(), encodedTable.getCount());
    }

    return SLANG_OK;
}

/* static */Result SerialContainerUtil::read(RiffContainer* container, const ReadOptions& options, SerialContainerData& out)
{
    out.clear();

    RiffContainer::ListChunk* containerChunk = container->getRoot()->findListRec(SerialBinary::kContainerFourCc);
    if (!containerChunk)
    {
        // Must be a container
        return SLANG_FAIL;
    }

    SerialBinary::ContainerHeader* containerHeader = containerChunk->findContainedData<SerialBinary::ContainerHeader>(SerialBinary::kContainerHeaderFourCc);
    if (!containerHeader)
    {
        // Didn't find the header
        return SLANG_FAIL;
    }

    const SerialCompressionType containerCompressionType = SerialCompressionType(containerHeader->compressionType);

    StringSlicePool containerStringPool(StringSlicePool::Style::Default);

    if (RiffContainer::Data* stringTableData = containerChunk->findContainedData(SerialBinary::kStringTableFourCc))
    {
        SerialStringTableUtil::decodeStringTable((const char*)stringTableData->getPayload(), stringTableData->getSize(), containerStringPool);
    }

    RefPtr<SerialSourceLocReader> sourceLocReader;
    RefPtr<SerialClasses> serialClasses;

    // Debug information
    if (auto debugChunk = containerChunk->findContainedList(SerialSourceLocData::kDebugFourCc))
    {
        // Read into data
        SerialSourceLocData sourceLocData;
        SLANG_RETURN_ON_FAIL(sourceLocData.readContainer(containerCompressionType, debugChunk));

        // Turn into DebugReader
        sourceLocReader = new SerialSourceLocReader;
        SLANG_RETURN_ON_FAIL(sourceLocReader->read(&sourceLocData, options.sourceManager));
    }

    // Add modules
    if (RiffContainer::ListChunk* moduleList = containerChunk->findContainedList(SerialBinary::kModuleListFourCc))
    {
        RiffContainer::Chunk* chunk = moduleList->getFirstContainedChunk();
        while (chunk)
        {
            auto startChunk = chunk;

            RefPtr<ASTBuilder> astBuilder;
            NodeBase* astRootNode = nullptr;
            RefPtr<IRModule> irModule;

            if (auto irChunk = as<RiffContainer::ListChunk>(chunk, IRSerialBinary::kIRModuleFourCc))
            {
                IRSerialData serialData;

                SLANG_RETURN_ON_FAIL(IRSerialReader::readContainer(irChunk, containerCompressionType, &serialData));

                // Read IR back from serialData
                IRSerialReader reader;
                SLANG_RETURN_ON_FAIL(reader.read(serialData, options.session, sourceLocReader, irModule));

                // Onto next chunk
                chunk = chunk->m_next;
            }

            if (auto astChunk = as<RiffContainer::ListChunk>(chunk, ASTSerialBinary::kSlangASTModuleFourCC))
            {
                RiffContainer::Data* astData = astChunk->findContainedData(ASTSerialBinary::kSlangASTModuleDataFourCC);

                if (astData)
                {
                    if (!serialClasses)
                    {
                        SLANG_RETURN_ON_FAIL(SerialClasses::create(serialClasses));
                    }

                    // TODO(JS): We probably want to store off better information about each of the translation unit
                    // including some kind of 'name'.
                    // For now we just generate a name.

                    StringBuilder buf;
                    buf << "tu" << out.modules.getCount();

                    astBuilder = new ASTBuilder(options.sharedASTBuilder, buf.ProduceString());

                    DefaultSerialObjectFactory objectFactory(astBuilder);

                    SerialReader reader(serialClasses, &objectFactory);

                    reader.getExtraObjects().set(sourceLocReader);

                    SLANG_RETURN_ON_FAIL(reader.load((const uint8_t*)astData->getPayload(), astData->getSize(), options.namePool));

                    // Get the root node. It's at index 1 (0 is the null value).
                    astRootNode = reader.getPointer(SerialIndex(1)).dynamicCast<NodeBase>();
                }

                // Onto next chunk
                chunk = chunk->m_next;
            }

            if (astBuilder || irModule)
            {
                SerialContainerData::Module module;

                module.astBuilder = astBuilder;
                module.astRootNode = astRootNode;
                module.irModule = irModule;

                out.modules.add(module);
            }

            // If no progress, step to next chunk
            chunk = (chunk == startChunk) ? chunk->m_next : chunk;
        }
    }

    // Add all the entry points
    {
        List<RiffContainer::DataChunk*> entryPointChunks;
        containerChunk->findContained(SerialBinary::kEntryPointFourCc, entryPointChunks);

        for (auto entryPointChunk : entryPointChunks)
        {
            auto reader = entryPointChunk->asReadHelper();

            auto readString = [&]()
            {
                uint32_t length = 0;
                reader.read(length);

                char* begin = (char*)reader.getData();
                reader.skip(length + 1);

                return UnownedStringSlice(begin, begin + length);
            };

            SerialContainerBinary::EntryPoint srcEntryPoint;
            SLANG_RETURN_ON_FAIL(reader.read(srcEntryPoint));

            SerialContainerData::EntryPoint dstEntryPoint;

            dstEntryPoint.name = options.namePool->getName(containerStringPool.getSlice(StringSlicePool::Handle(srcEntryPoint.name)));
            dstEntryPoint.profile.raw = srcEntryPoint.profile;
            dstEntryPoint.mangledName = containerStringPool.getSlice(StringSlicePool::Handle(srcEntryPoint.mangledName));

            out.entryPoints.add(dstEntryPoint);
        }
    }

    return SLANG_OK;
}

/* static */SlangResult SerialContainerUtil::verifyIRSerialize(IRModule* module, Session* session, const WriteOptions& options)
{
    // Verify if we can stream out with raw source locs

    List<IRInst*> originalInsts;
    IRSerialWriter::calcInstructionList(module, originalInsts);

    IRSerialData irData;

    OwnedMemoryStream memoryStream(FileAccess::ReadWrite);

    {
        RiffContainer riffContainer;

        // Need to put all of this in a container 
        RiffContainer::ScopeChunk containerScope(&riffContainer, RiffContainer::Chunk::Kind::List, SerialBinary::kContainerFourCc);

        RefPtr<SerialSourceLocWriter> sourceLocWriter;

        if (options.optionFlags & SerialOptionFlag::DebugInfo)
        {
            sourceLocWriter = new SerialSourceLocWriter(options.sourceManager);
        }
        
        {
            // Write IR out to serialData - copying over SourceLoc information directly
            IRSerialWriter writer;
            SLANG_RETURN_ON_FAIL(writer.write(module, sourceLocWriter, options.optionFlags, &irData));
        }
        SLANG_RETURN_ON_FAIL(IRSerialWriter::writeContainer(irData, options.compressionType, &riffContainer));
    
        // Write the debug info Riff container
        if (sourceLocWriter)
        {
            SerialSourceLocData serialData;
            sourceLocWriter->write(&serialData);

            SLANG_RETURN_ON_FAIL(serialData.writeContainer(options.compressionType, &riffContainer));
        }

        SLANG_RETURN_ON_FAIL(RiffUtil::write(&riffContainer, &memoryStream));
    }

    // Reset stream
    memoryStream.seek(SeekOrigin::Start, 0);

    SourceManager workSourceManager;
    workSourceManager.initialize(options.sourceManager, nullptr);

    // The read ir module
    RefPtr<IRModule> irReadModule;
    {
        RiffContainer riffContainer;
        SLANG_RETURN_ON_FAIL(RiffUtil::read(&memoryStream, riffContainer));

        RiffContainer::ListChunk* rootList = riffContainer.getRoot();

        RefPtr<SerialSourceLocReader> sourceLocReader;

        // If we have debug info then find and read it
        if (options.optionFlags & SerialOptionFlag::DebugInfo)
        {
            RiffContainer::ListChunk* debugList = rootList->findContainedList(SerialSourceLocData::kDebugFourCc);
            if (!debugList)
            {
                return SLANG_FAIL;
            }
            SerialSourceLocData sourceLocData;
            SLANG_RETURN_ON_FAIL(sourceLocData.readContainer(options.compressionType, debugList));

            sourceLocReader = new SerialSourceLocReader;
            SLANG_RETURN_ON_FAIL(sourceLocReader->read(&sourceLocData, &workSourceManager));
        }

        {
            RiffContainer::ListChunk* irList = rootList->findContainedList(IRSerialBinary::kIRModuleFourCc);
            if (!irList)
            {
                return SLANG_FAIL;
            }

            {
                IRSerialData irReadData;
                IRSerialReader reader;
                SLANG_RETURN_ON_FAIL(reader.readContainer(irList, options.compressionType, &irReadData));

                // Check the stream read data is the same
                if (irData != irReadData)
                {
                    SLANG_ASSERT(!"Streamed in data doesn't match");
                    return SLANG_FAIL;
                }

                SLANG_RETURN_ON_FAIL(reader.read(irData, session, sourceLocReader, irReadModule));
            }
        }
    }

    List<IRInst*> readInsts;
    IRSerialWriter::calcInstructionList(irReadModule, readInsts);

    if (readInsts.getCount() != originalInsts.getCount())
    {
        SLANG_ASSERT(!"Instruction counts don't match");
        return SLANG_FAIL;
    }

    if (options.optionFlags & SerialOptionFlag::RawSourceLocation)
    {
        SLANG_ASSERT(readInsts[0] == originalInsts[0]);
        // All the source locs should be identical
        for (Index i = 1; i < readInsts.getCount(); ++i)
        {
            IRInst* origInst = originalInsts[i];
            IRInst* readInst = readInsts[i];

            if (origInst->sourceLoc.getRaw() != readInst->sourceLoc.getRaw())
            {
                SLANG_ASSERT(!"Source locs don't match");
                return SLANG_FAIL;
            }
        }
    }
    else if (options.optionFlags & SerialOptionFlag::DebugInfo)
    {
        // They should be on the same line nos
        for (Index i = 1; i < readInsts.getCount(); ++i)
        {
            IRInst* origInst = originalInsts[i];
            IRInst* readInst = readInsts[i];

            if (origInst->sourceLoc.getRaw() == readInst->sourceLoc.getRaw())
            {
                continue;
            }

            // Work out the
            SourceView* origSourceView = options.sourceManager->findSourceView(origInst->sourceLoc);
            SourceView* readSourceView = workSourceManager.findSourceView(readInst->sourceLoc);

            // if both are null we are done
            if (origSourceView == nullptr && origSourceView == readSourceView)
            {
                continue;
            }
            SLANG_ASSERT(origSourceView && readSourceView);

            // The offset should be the same
            Index origOffset = origInst->sourceLoc.getRaw() - origSourceView->getRange().begin.getRaw();
            Index readOffset = readInst->sourceLoc.getRaw() - readSourceView->getRange().begin.getRaw();

            if (origOffset != readOffset)
            {
                SLANG_ASSERT(!"SourceLoc offset debug data didn't match");
                return SLANG_FAIL;
            }

            {
                auto origInfo = origSourceView->getHumaneLoc(origInst->sourceLoc, SourceLocType::Actual);
                auto readInfo = readSourceView->getHumaneLoc(readInst->sourceLoc, SourceLocType::Actual);

                if (!(origInfo.line == readInfo.line && origInfo.column == readInfo.column && origInfo.pathInfo.foundPath == readInfo.pathInfo.foundPath))
                {
                    SLANG_ASSERT(!"Debug data didn't match");
                    return SLANG_FAIL;
                }
            }

            // We may have adjusted line numbers -> but they may not match, because we only reconstruct one view
            // So for now disable this test

            if (false)
            {
                auto origInfo = origSourceView->getHumaneLoc(origInst->sourceLoc, SourceLocType::Nominal);
                auto readInfo = readSourceView->getHumaneLoc(readInst->sourceLoc, SourceLocType::Nominal);

                if (!(origInfo.line == readInfo.line && origInfo.column == readInfo.column && origInfo.pathInfo.foundPath == readInfo.pathInfo.foundPath))
                {
                    SLANG_ASSERT(!"Debug data didn't match");
                    return SLANG_FAIL;
                }
            }
        }
    }

    return SLANG_OK;
}

} // namespace Slang