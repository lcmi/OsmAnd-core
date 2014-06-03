#include "MapRendererTiledSymbolsResource.h"

#include "MapRenderer.h"
#include "MapRendererResourcesManager.h"
#include "IMapDataProvider.h"
#include "IMapTiledSymbolsProvider.h"
#include "MapSymbolsGroupShareableById.h"
#include "MapRendererResourcesManager.h"
#include "MapRendererBaseResourcesCollection.h"
#include "MapRendererTiledSymbolsResourcesCollection.h"
#include "QKeyValueIterator.h"
#include "Utilities.h"

//#define OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE 1
#ifndef OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE
#   define OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE 0
#endif // !defined(OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE)

OsmAnd::MapRendererTiledSymbolsResource::MapRendererTiledSymbolsResource(
    MapRendererResourcesManager* owner,
    const TiledEntriesCollection<MapRendererBaseTiledResource>& collection,
    const TileId tileId,
    const ZoomLevel zoom)
    : MapRendererBaseTiledResource(owner, MapRendererResourceType::Symbols, collection, tileId, zoom)
{
}

OsmAnd::MapRendererTiledSymbolsResource::~MapRendererTiledSymbolsResource()
{
    safeUnlink();
}

bool OsmAnd::MapRendererTiledSymbolsResource::obtainData(bool& dataAvailable, const IQueryController* queryController)
{
    // Obtain collection link and maintain it
    const auto link_ = link.lock();
    if (!link_)
        return false;
    const auto collection = static_cast<MapRendererTiledSymbolsResourcesCollection*>(&link_->collection);

    // Get source of tile
    std::shared_ptr<IMapDataProvider> provider_;
    bool ok = owner->obtainProviderFor(static_cast<MapRendererBaseResourcesCollection*>(collection), provider_);
    if (!ok)
        return false;
    const auto provider = std::static_pointer_cast<IMapTiledSymbolsProvider>(provider_);

    auto& sharedGroupsResources = collection->_sharedGroupsResources[zoom];

    // Obtain tile from provider
    QList< std::shared_ptr<GroupResources> > referencedSharedGroupsResources;
    QList< proper::shared_future< std::shared_ptr<GroupResources> > > futureReferencedSharedGroupsResources;
    QSet< uint64_t > loadedSharedGroups;
    std::shared_ptr<MapTiledData> tile_;
    const auto requestSucceeded = provider->obtainData(tileId, zoom, tile_,
        [this, provider, &sharedGroupsResources, &referencedSharedGroupsResources, &futureReferencedSharedGroupsResources, &loadedSharedGroups]
        (const IMapTiledSymbolsProvider*, const std::shared_ptr<const MapSymbolsGroup>& symbolsGroup_) -> bool
        {
            const auto symbolsGroup = std::dynamic_pointer_cast<const MapSymbolsGroupShareableById>(symbolsGroup_);

            // If not shareable, just accept it
            if (!symbolsGroup)
                return true;

            // Check if this shared symbol is already available, or mark it as pending
            std::shared_ptr<GroupResources> sharedGroupResources;
            proper::shared_future< std::shared_ptr<GroupResources> > futureSharedGroupResources;
            if (sharedGroupsResources.obtainReferenceOrFutureReferenceOrMakePromise(symbolsGroup->id, sharedGroupResources, futureSharedGroupResources))
            {
                if (static_cast<bool>(sharedGroupResources))
                {
                    referencedSharedGroupsResources.push_back(qMove(sharedGroupResources));

#if OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE
                    const auto shareableById = std::static_pointer_cast<const MapSymbolsGroupShareableById>(sharedGroupResources->group);
                    LogPrintf(LogSeverityLevel::Debug,
                        "Shared GroupResources(%p) for MapObject #%" PRIu64 " (%" PRIi64 ") referenced from %p (%dx%d@%d)",
                        sharedGroupResources.get(),
                        shareableById->id,
                        static_cast<int64_t>(shareableById->id) / 2,
                        this,
                        tileId.x, tileId.y, zoom);
#endif // OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE
                }
                else
                    futureReferencedSharedGroupsResources.push_back(qMove(futureSharedGroupResources));
                return false;
            }

            // Or load this shared group
            loadedSharedGroups.insert(symbolsGroup->id);
            return true;
        });
    if (!requestSucceeded)
        return false;
    const auto tile = std::static_pointer_cast<TiledMapSymbolsData>(tile_);

    // Store data
    _sourceData = tile;
    dataAvailable = static_cast<bool>(tile);

    // Process data
    if (!dataAvailable)
        return true;

    // Move referenced shared groups
    _referencedSharedGroupsResources = referencedSharedGroupsResources;

    // tile->symbolsGroups contains groups that derived from unique symbols, or loaded shared groups
    for (const auto& group : constOf(tile->symbolsGroups))
    {
        std::shared_ptr<GroupResources> groupResources(new GroupResources(group));

        if (const auto shareableById = std::dynamic_pointer_cast<const MapSymbolsGroupShareableById>(group))
        {
            // Check if this group is loaded as shared
            if (!loadedSharedGroups.contains(shareableById->id))
            {
                _uniqueGroupsResources.push_back(qMove(groupResources));
                continue;
            }

            // Otherwise insert it as shared group
            sharedGroupsResources.fulfilPromiseAndReference(shareableById->id, groupResources);
            _referencedSharedGroupsResources.push_back(qMove(groupResources));

#if OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE
            LogPrintf(LogSeverityLevel::Debug,
                "Shared GroupResources(%p) for MapObject #%" PRIu64 " (%" PRIi64 ") allocated and referenced from %p (%dx%d@%d): %" PRIu64 " ref(s)",
                groupResources.get(),
                shareableById->id,
                static_cast<int64_t>(shareableById->id) / 2,
                this,
                tileId.x, tileId.y, zoom,
                sharedGroupsResources.getReferencesCount(shareableById->id));
#endif // OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE
        }
        else
        {
            _uniqueGroupsResources.push_back(groupResources);
        }
    }

    // Wait for future referenced shared groups
    for (auto& futureGroup : constOf(futureReferencedSharedGroupsResources))
    {
        auto groupResources = futureGroup.get();

        _referencedSharedGroupsResources.push_back(qMove(groupResources));

#if OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE
        const auto shareableById = std::static_pointer_cast<const MapSymbolsGroupShareableById>(groupResources->group);
        LogPrintf(LogSeverityLevel::Debug,
            "Shared GroupResources(%p) for MapObject #%" PRIu64 " (%" PRIi64 ") referenced from %p (%dx%d@%d): %" PRIu64 " ref(s)",
            groupResources.get(),
            shareableById->id,
            static_cast<int64_t>(shareableById->id) / 2,
            this,
            tileId.x, tileId.y, zoom,
            sharedGroupsResources.getReferencesCount(shareableById->id));
#endif // OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE
    }

    // Register all obtained symbols
    const auto& self = shared_from_this();
    for (const auto& groupResources : constOf(_uniqueGroupsResources))
    {
        for (const auto& symbol : constOf(groupResources->group->symbols))
            owner->registerMapSymbol(symbol, self);
    }
    for (const auto& groupResources : constOf(_referencedSharedGroupsResources))
    {
        for (const auto& symbol : constOf(groupResources->group->symbols))
            owner->registerMapSymbol(symbol, self);
    }

    // Since there's a copy of references to map symbols groups and symbols themselves,
    // it's safe to consume all the data here
    _sourceData->releaseConsumableContent();

    return true;
}

bool OsmAnd::MapRendererTiledSymbolsResource::uploadToGPU()
{
    typedef std::pair< std::shared_ptr<MapSymbol>, std::shared_ptr<const GPUAPI::ResourceInGPU> > SymbolResourceEntry;
    typedef std::pair< std::shared_ptr<MapSymbol>, proper::shared_future< std::shared_ptr<const GPUAPI::ResourceInGPU> > > FutureSymbolResourceEntry;

    bool ok;
    bool anyUploadFailed = false;

    const auto link_ = link.lock();
    const auto collection = static_cast<MapRendererTiledSymbolsResourcesCollection*>(&link_->collection);

    // Unique
    QMultiHash< std::shared_ptr<GroupResources>, SymbolResourceEntry > uniqueUploaded;
    for (const auto& groupResources : constOf(_uniqueGroupsResources))
    {
        for (const auto& symbol : constOf(groupResources->group->symbols))
        {
            // Prepare data and upload to GPU
            assert(static_cast<bool>(symbol->bitmap));
            std::shared_ptr<const GPUAPI::ResourceInGPU> resourceInGPU;
            ok = owner->uploadSymbolToGPU(symbol, resourceInGPU);

            // If upload have failed, stop
            if (!ok)
            {
                LogPrintf(LogSeverityLevel::Error, "Failed to upload unique symbol (size %dx%d) in %dx%d@%d tile",
                    symbol->bitmap->width(), symbol->bitmap->height(),
                    tileId.x, tileId.y, zoom);

                anyUploadFailed = true;
                break;
            }

            // Mark this symbol as uploaded
            uniqueUploaded.insertMulti(groupResources, qMove(SymbolResourceEntry(symbol, resourceInGPU)));
        }
        if (anyUploadFailed)
            break;
    }

    // Shared
    QMultiHash< std::shared_ptr<GroupResources>, SymbolResourceEntry > sharedUploaded;
    for (const auto& groupResources : constOf(_referencedSharedGroupsResources))
    {
        if (groupResources->group->symbols.isEmpty())
            continue;

        // This check means "continue if shared symbols were uploaded by other resource"
        // This check needs no special handling, since all GPU resources work is done from same thread.
        if (!groupResources->resourcesInGPU.isEmpty())
            continue;

        for (const auto& symbol : constOf(groupResources->group->symbols))
        {
            // Prepare data and upload to GPU
            assert(static_cast<bool>(symbol->bitmap));
            std::shared_ptr<const GPUAPI::ResourceInGPU> resourceInGPU;
            ok = owner->uploadSymbolToGPU(symbol, resourceInGPU);

            // If upload have failed, stop
            if (!ok)
            {
                LogPrintf(LogSeverityLevel::Error, "Failed to upload unique symbol (size %dx%d) in %dx%d@%d tile",
                    symbol->bitmap->width(), symbol->bitmap->height(),
                    tileId.x, tileId.y, zoom);

                anyUploadFailed = true;
                break;
            }

            // Mark this symbol as uploaded
            sharedUploaded.insertMulti(groupResources, qMove(SymbolResourceEntry(symbol, resourceInGPU)));
        }
        if (anyUploadFailed)
            break;
    }

    // If at least one symbol failed to upload, consider entire tile as failed to upload,
    // and unload its partial GPU resources
    if (anyUploadFailed)
    {
        uniqueUploaded.clear();
        sharedUploaded.clear();

        return false;
    }

    // All resources have been uploaded to GPU successfully by this point,
    // so it's safe to walk across symbols and remove bitmaps:

    // Unique
    for (const auto& entry : rangeOf(constOf(uniqueUploaded)))
    {
        const auto& groupResources = entry.key();
        const auto& symbol = entry.value().first;
        auto& resource = entry.value().second;

        // Unload bitmap from symbol, since it's uploaded already
        symbol->bitmap.reset();

        // Add GPU resource reference
        _resourcesInGPU.insert(symbol, resource);
        groupResources->resourcesInGPU.insert(qMove(symbol), qMove(resource));
    }

    // Shared
    for (const auto& entry : rangeOf(constOf(sharedUploaded)))
    {
        const auto& groupResources = entry.key();
        auto symbol = entry.value().first;
        auto& resource = entry.value().second;

        // Unload bitmap from symbol, since it's uploaded already
        symbol->bitmap.reset();

        // Add GPU resource reference
        _resourcesInGPU.insert(symbol, resource);
        groupResources->resourcesInGPU.insert(qMove(symbol), qMove(resource));
    }

    return true;
}

void OsmAnd::MapRendererTiledSymbolsResource::unloadFromGPU()
{
    const auto link_ = link.lock();
    const auto collection = static_cast<MapRendererTiledSymbolsResourcesCollection*>(&link_->collection);

    // Remove quick references
    _resourcesInGPU.clear();

    // Unique
    for (const auto& groupResources : constOf(_uniqueGroupsResources))
    {
        // Unload map symbol resources from GPU
#if OSMAND_DEBUG
        for (const auto& entryResourceInGPU : rangeOf(groupResources->resourcesInGPU))
        {
            const auto& symbol = entryResourceInGPU.key();
            auto& resourceInGPU = entryResourceInGPU.value();

            assert(resourceInGPU.use_count() == 1);
            resourceInGPU.reset();
        }
#endif

        // For unique group resources it's safe to clear 'MapSymbol->ResourceInGPU' map
        groupResources->resourcesInGPU.clear();
    }
    // Do not clear _uniqueGroupsResources list, since it will be used to unregister symbols

    // Shared
    auto& sharedGroupsResources = collection->_sharedGroupsResources[zoom];
    for (auto& groupResources : _referencedSharedGroupsResources)
    {
        const auto shareableById = std::static_pointer_cast<const MapSymbolsGroupShareableById>(groupResources->group);

        bool wasRemoved = false;
        uintmax_t* pRefsRemaining = nullptr;
#if OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE
        uintmax_t refsRemaining = 0;
        pRefsRemaining = &refsRemaining;
#endif // OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE

        // Release reference first, since GroupResources will be released after GPU resource will be dereferenced,
        // other tile may catch empty non-loadable GroupResources.
        auto groupResources_ = groupResources;
        sharedGroupsResources.releaseReference(shareableById->id, groupResources_, true, &wasRemoved, pRefsRemaining);

#if OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE
        LogPrintf(LogSeverityLevel::Debug,
            "Shared GroupResources(%p) dereferenced for MapObject #%" PRIu64 " (%" PRIi64 ") in %p (%dx%d@%d): %" PRIu64 " ref(s) remain, %s",
            groupResources.get(),
            shareableById->id,
            static_cast<int64_t>(shareableById->id) / 2,
            this,
            tileId.x, tileId.y, zoom,
            static_cast<uint64_t>(refsRemaining),
            wasRemoved ? "removed" : "not removed");
#endif // OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE

        // In case final reference to shared group resources was removed, it's safe to clear resources,
        // since no-one else will check for this map being empty. Otherwise, these resources are still needed
        // somewhere
        if (wasRemoved)
            groupResources->resourcesInGPU.clear();
    }
    // Do not clear _referencedSharedGroupsResources list, since it will be used to unregister symbols
}

void OsmAnd::MapRendererTiledSymbolsResource::releaseData()
{
    const auto link_ = link.lock();
    const auto collection = static_cast<MapRendererTiledSymbolsResourcesCollection*>(&link_->collection);
    const auto& self = shared_from_this();

    // Remove quick references (if any left)
    _resourcesInGPU.clear();

    // Unique
    for (const auto& groupResources : constOf(_uniqueGroupsResources))
    {
        // Unregister symbols
        for (const auto& symbol : constOf(groupResources->group->symbols))
            owner->unregisterMapSymbol(symbol, self);

        // In case GPU resources was not released before, perform this operation here
        // Otherwise, groupResources->resourcesInGPU array is empty for unique group resources
        for (const auto& entryResourceInGPU : rangeOf(groupResources->resourcesInGPU))
        {
            const auto& symbol = entryResourceInGPU.key();
            auto& resourceInGPU = entryResourceInGPU.value();

            // Unload symbol from GPU thread (using dispatcher)
            assert(resourceInGPU.use_count() == 1);
            auto resourceInGPU_ = qMove(resourceInGPU);
            owner->renderer->getGpuThreadDispatcher().invokeAsync(
                [resourceInGPU_]
                () mutable
                {
                    assert(resourceInGPU_.use_count() == 1);
                    resourceInGPU_.reset();
                });
#ifndef Q_COMPILER_RVALUE_REFS
            resourceInGPU.reset();
#endif //!Q_COMPILER_RVALUE_REFS
        }
    }
    // Now it's safe to clear _uniqueGroupsResources list,
    // since after symbols are unregistered, it's not needed anymore
    _uniqueGroupsResources.clear();

    // Shared
    auto& sharedGroupsResources = collection->_sharedGroupsResources[zoom];
    for (auto& groupResources : _referencedSharedGroupsResources)
    {
        // Unregister symbols
        for (const auto& symbol : constOf(groupResources->group->symbols))
            owner->unregisterMapSymbol(symbol, self);

        // For shared group resources groupResources->resourcesInGPU is going to be empty only in case
        // group is not referenced from anywhere. In that case just skip dereferencing
        if (groupResources->resourcesInGPU.isEmpty())
            continue;

        // Otherwise, perform dereferencing
        const auto shareableById = std::static_pointer_cast<const MapSymbolsGroupShareableById>(groupResources->group);
        uintmax_t* pRefsRemaining = nullptr;
#if OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE
        uintmax_t refsRemaining = 0;
        pRefsRemaining = &refsRemaining;
#endif // OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE
        bool wasRemoved = false;
        auto groupResources_ = groupResources;
        sharedGroupsResources.releaseReference(shareableById->id, groupResources_, true, &wasRemoved, pRefsRemaining);

#if OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE
        LogPrintf(LogSeverityLevel::Debug,
            "Shared GroupResources(%p) dereferenced for MapObject #%" PRIu64 " (%" PRIi64 ") in %p (%dx%d@%d): %" PRIu64 " ref(s) remain, %s",
            groupResources.get(),
            shareableById->id,
            static_cast<int64_t>(shareableById->id) / 2,
            this,
            tileId.x, tileId.y, zoom,
            static_cast<uint64_t>(refsRemaining),
            wasRemoved ? "removed" : "not removed");
#endif // OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE
        
        // GPU resources of a shared group resources should be unloaded only in case this was the last
        // reference to shared group resources
        if (!wasRemoved)
            continue;

        // If this was the last reference to those shared group resources, it's safe to unload
        // all GPU resources assigned to it. And clear the entire mapping, since it's not needed
        // by anyone anywhere
        for (const auto& entryResourceInGPU : rangeOf(groupResources->resourcesInGPU))
        {
            const auto& symbol = entryResourceInGPU.key();
            auto& resourceInGPU = entryResourceInGPU.value();

            // Unload symbol from GPU thread (using dispatcher)
            assert(resourceInGPU.use_count() == 1);
            auto resourceInGPU_ = qMove(resourceInGPU);
            owner->renderer->getGpuThreadDispatcher().invokeAsync(
                [resourceInGPU_]
                () mutable
                {
                    assert(resourceInGPU_.use_count() == 1);
                    resourceInGPU_.reset();
                });
#ifndef Q_COMPILER_RVALUE_REFS
            resourceInGPU.reset();
#endif //!Q_COMPILER_RVALUE_REFS
        }
        groupResources->resourcesInGPU.clear();
    }
    // Now it's safe to clear _uniqueGroupsResources list,
    // since after symbols are unregistered, it's not needed anymore
    _referencedSharedGroupsResources.clear();

    _sourceData.reset();
}

std::shared_ptr<const OsmAnd::GPUAPI::ResourceInGPU> OsmAnd::MapRendererTiledSymbolsResource::getGpuResourceFor(const std::shared_ptr<const MapSymbol>& mapSymbol)
{
    const auto citResourceInGPU = _resourcesInGPU.constFind(mapSymbol);
    if (citResourceInGPU == _resourcesInGPU.cend())
        return nullptr;
    return *citResourceInGPU;
}

OsmAnd::MapRendererTiledSymbolsResource::GroupResources::GroupResources(const std::shared_ptr<MapSymbolsGroup>& group_)
    : group(group_)
{
}

OsmAnd::MapRendererTiledSymbolsResource::GroupResources::~GroupResources()
{
#if OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE
    LogPrintf(LogSeverityLevel::Debug, "Shared GroupResources(%p) destroyed", this);
#endif // OSMAND_LOG_SHARED_MAP_SYMBOLS_GROUPS_LIFECYCLE
}