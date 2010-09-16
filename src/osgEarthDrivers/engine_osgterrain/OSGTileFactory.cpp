/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2010 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include "OSGTileFactory"
#include "CustomTerrain"
#include "FileLocationCallback"
#include "TerrainTileEdgeNormalizerUpdateCallback"

#include <osgEarth/Map>
#include <osgEarth/Caching>
#include <osgEarth/HeightFieldUtils>
#include <osgEarth/Registry>
#include <osgEarth/ImageUtils>
#include <osgEarth/TileSource>

#include <osg/Image>
#include <osg/Notify>
#include <osg/PagedLOD>
#include <osg/ClusterCullingCallback>
#include <osg/CoordinateSystemNode>
#include <osgFX/MultiTextureControl>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgTerrain/Terrain>
#include <osgTerrain/TerrainTile>
#include <osgTerrain/Locator>
#include <osgTerrain/GeometryTechnique>
#include <OpenThreads/ReentrantMutex>
#include <sstream>
#include <stdlib.h>

using namespace OpenThreads;
using namespace osgEarth;
using namespace osgEarth::Drivers;

#define LC "[OSGTileFactory] "

/*****************************************************************************/

struct TileImageBackfillCallback : public osg::NodeCallback
{
    void operator()( osg::Node* node, osg::NodeVisitor* nv )
    {
        if ( nv->getVisitorType() == osg::NodeVisitor::CULL_VISITOR )
        {
            if ( node->asGroup()->getNumChildren() > 0 )
            {
                osg::ref_ptr<CustomTile> tile = dynamic_cast<CustomTile*>( node->asGroup()->getChild(0) );
                if ( tile.valid() && tile->getUseLayerRequests() )
                {
                    tile->servicePendingImageRequests( nv->getFrameStamp()->getFrameNumber() );
                }
            }
        }
        traverse( node, nv );
    }
};

/*****************************************************************************/

OSGTileFactory::OSGTileFactory( unsigned int engineId, const OSGTerrainOptions& props ) :
osg::Referenced( true ),
_engineId( engineId ),
_terrainOptions( props )
{
    init();
}

void
OSGTileFactory::init()
{
    const char* useL2 = ::getenv("OSGEARTH_L2_CACHE");
    _L2cache = useL2 && useL2[0] != 0L ? new L2Cache() : 0L;

    LoadingPolicy::Mode mode = _terrainOptions.loadingPolicy()->mode().value();
    OE_INFO << LC << "Loading policy mode = " <<
        ( mode == LoadingPolicy::MODE_PREEMPTIVE ? "preemptive" :
        mode == LoadingPolicy::MODE_SEQUENTIAL ? "sequential" :
        "standard" )
        << ", threads per core = " << _terrainOptions.loadingPolicy()->numThreadsPerCore().value()
        << std::endl;
}

const OSGTerrainOptions& 
OSGTileFactory::getTerrainOptions() const
{
    return _terrainOptions;
}

std::string
OSGTileFactory::createURI( unsigned int id, const TileKey& key )
{
    std::stringstream ss;
    ss << key.str() << "." <<id<<".osgearth_osgterrain_tile";
    std::string ssStr;
    ssStr = ss.str();
    return ssStr;
}

// Make a MatrixTransform suitable for use with a Locator object based on the given extents.
// Calling Locator::setTransformAsExtents doesn't work with OSG 2.6 due to the fact that the
// _inverse member isn't updated properly.  Calling Locator::setTransform works correctly.
osg::Matrixd
OSGTileFactory::getTransformFromExtents(double minX, double minY, double maxX, double maxY) const
{
    osg::Matrixd transform;
    transform.set(
        maxX-minX, 0.0,       0.0, 0.0,
        0.0,       maxY-minY, 0.0, 0.0,
        0.0,       0.0,       1.0, 0.0,
        minX,      minY,      0.0, 1.0); 
    return transform;
}

osg::Node*
OSGTileFactory::createSubTiles( Map* map, CustomTerrain* terrain, const TileKey& key, bool populateLayers )
{
    //OE_NOTICE << "createSubTiles" << std::endl;
    TileKey k0 = key.createChildKey(0);
    TileKey k1 = key.createChildKey(1);
    TileKey k2 = key.createChildKey(2);
    TileKey k3 = key.createChildKey(3);

    bool hasValidData = false;
    bool validData;

    bool fallback = false;
    osg::ref_ptr<osg::Node> q0 = createTile( map, terrain, k0, populateLayers, true, fallback, validData);
    if (!hasValidData && validData) hasValidData = true;

    osg::ref_ptr<osg::Node> q1 = createTile( map, terrain, k1, populateLayers, true, fallback, validData );
    if (!hasValidData && validData) hasValidData = true;

    osg::ref_ptr<osg::Node> q2 = createTile( map, terrain, k2, populateLayers, true, fallback, validData );
    if (!hasValidData && validData) hasValidData = true;

    osg::ref_ptr<osg::Node> q3 = createTile( map, terrain, k3, populateLayers, true, fallback, validData );
    if (!hasValidData && validData) hasValidData = true;

    if (!hasValidData)
    {
        OE_DEBUG << LC << "Couldn't create any quadrants for " << key.str() << " time to stop subdividing!" << std::endl;
        return NULL;
    }

    osg::Group* tile_parent = new osg::Group();

    fallback = true;
    //Fallback on tiles if we couldn't create any
    if (!q0.valid())
    {
        q0 = createTile( map, terrain, k0, populateLayers, true, fallback, validData);
    }

    if (!q1.valid())
    {
        q1 = createTile( map, terrain, k1, populateLayers, true, fallback, validData);
    }

    if (!q2.valid())
    {
        q2 = createTile( map, terrain, k2, populateLayers, true, fallback, validData);
    }

    if (!q3.valid())
    {        
        q3 = createTile( map, terrain, k3, populateLayers, true, fallback, validData);
    }

    tile_parent->addChild( q0.get() );
    tile_parent->addChild( q1.get() );
    tile_parent->addChild( q2.get() );
    tile_parent->addChild( q3.get() );
    return tile_parent;
}

GeoImage*
OSGTileFactory::createValidGeoImage(MapLayer* layer,
                                    const TileKey& key,
                                    ProgressCallback* progress)
{
    //TODO:  Redo this to just grab images from the parent TerrainTiles
    //Try to create the image with the given key
    TileKey image_key = key;

    osg::ref_ptr<GeoImage> geo_image;

    while (image_key.valid())
    {
        if ( layer->isKeyValid(image_key) )
        {
            geo_image = layer->createImage( image_key, progress );
            if (geo_image.valid()) return geo_image.release();
        }
        image_key = image_key.createParentKey();
    }
    return geo_image.release();
}

bool
OSGTileFactory::hasMoreLevels( Map* map, const TileKey& key )
{
    Threading::ScopedReadLock lock( map->getMapDataMutex() );

    bool more_levels = false;
    int max_level = 0;

    for ( MapLayerList::const_iterator i = map->getImageMapLayers().begin(); i != map->getImageMapLayers().end(); i++ )
    {
        if ( !i->get()->maxLevel().isSet() || key.getLevelOfDetail() < i->get()->maxLevel().get() )
        {
            more_levels = true;
            break;
        }
    }
    if ( !more_levels )
    {
        for( MapLayerList::const_iterator j = map->getHeightFieldMapLayers().begin(); j != map->getHeightFieldMapLayers().end(); j++ )
        {
            if ( !j->get()->maxLevel().isSet() || key.getLevelOfDetail() < j->get()->maxLevel().get() )
            {
                more_levels = true;
                break;
            }
        }
    }

    return more_levels;
}

bool
OSGTileFactory::isCached(Map* map, const osgEarth::TileKey& key)
{
    Threading::ScopedReadLock lock( map->getMapDataMutex() );

    const Profile* mapProfile = key.getProfile();

    //Check the imagery layers
    for( MapLayerList::const_iterator i = map->getImageMapLayers().begin(); i != map->getImageMapLayers().end(); i++ )
    {
        MapLayer* layer = i->get();
        osg::ref_ptr< Cache > cache = layer->getCache();
        if (!cache.valid()) return false;

        std::vector< TileKey > keys;

        if ( map->getProfile()->isEquivalentTo( layer->getProfile() ) )
        {
            keys.push_back( key );
        }
        else
        {
            layer->getProfile()->getIntersectingTiles( key, keys );
        }

        for (unsigned int j = 0; j < keys.size(); ++j)
        {
            if ( layer->isKeyValid( keys[j] ) )
            {
                if ( !cache->isCached( keys[j], layer->getName(), layer->cacheFormat().value() ) )
                {
                    return false;
                }
            }
        }
    }

    //Check the elevation layers
    for( MapLayerList::const_iterator i = map->getHeightFieldMapLayers().begin(); i != map->getHeightFieldMapLayers().end(); i++ )
    {
        MapLayer* layer = i->get();
        osg::ref_ptr< Cache > cache = layer->getCache();
        if (!cache.valid()) return false;

        std::vector<TileKey> keys;

        if ( map->getProfile()->isEquivalentTo( layer->getProfile() ) )
        {
            keys.push_back( key );
        }
        else
        {
            layer->getProfile()->getIntersectingTiles( key, keys );
        }

        for (unsigned int j = 0; j < keys.size(); ++j)
        {
            if ( layer->isKeyValid( keys[j] ) )
            {
                if ( !cache->isCached( keys[j], layer->getName(), layer->cacheFormat().value() ) )
                {
                    return false;
                }
            }
        }
    }
    return true;
}

osg::HeightField*
OSGTileFactory::createEmptyHeightField( const TileKey& key, int numCols, int numRows )
{
    osg::HeightField* hf = key.getProfile()->getVerticalSRS()->createReferenceHeightField(
        key.getGeoExtent(), numCols, numRows );

    return hf;

    ////Get the bounds of the key
    //double minx, miny, maxx, maxy;
    //key.getGeoExtent().getBounds(minx, miny, maxx, maxy);

    //osg::HeightField *hf = new osg::HeightField();
    //hf->allocate( numCols, numRows );
    //for(unsigned int i=0; i<hf->getHeightList().size(); i++ )
    //    hf->getHeightList()[i] = 0.0;

    //hf->setOrigin( osg::Vec3d( minx, miny, 0.0 ) );
    //hf->setXInterval( (maxx - minx)/(double)(hf->getNumColumns()-1) );
    //hf->setYInterval( (maxy - miny)/(double)(hf->getNumRows()-1) );
    //hf->setBorderWidth( 0 );
    //return hf;
}

void
OSGTileFactory::addPlaceholderImageLayers(CustomTile* tile,
                                          CustomTile* ancestorTile,
                                          const MapLayerList& imageMapLayers,
                                          GeoLocator* defaultLocator,
                                          const TileKey& key)
{
    if ( !ancestorTile )
    {
        //OE_NOTICE << "No ancestorTile for key " << key.str() << std::endl;
        return;
    }        

    // Now if we have a valid ancestor tile, go through and make a temporary tile consisting only of
    // layers that exist in the new map layer image list as well.
    //int layer = 0;
    for( unsigned int j=0; j<ancestorTile->getNumColorLayers(); j++ )
    {
        tile->setColorLayer( j,  ancestorTile->getColorLayer( j ) );
    }
}


void
OSGTileFactory::addPlaceholderHeightfieldLayer(CustomTile* tile,
                                               CustomTile* ancestorTile,
                                               GeoLocator* defaultLocator,
                                               const TileKey& key,
                                               const TileKey& ancestorKey)
{
    osgTerrain::HeightFieldLayer* newHFLayer = 0L;

    if ( ancestorTile && ancestorKey.valid() )
    {
        osg::ref_ptr<osgTerrain::HeightFieldLayer> ancestorLayer = dynamic_cast<osgTerrain::HeightFieldLayer*>(ancestorTile->getElevationLayer());
        if ( ancestorLayer.valid() )
        {
            osg::ref_ptr<osg::HeightField> ancestorHF = ancestorLayer->getHeightField();
            if ( ancestorHF.valid() )
            {
                osg::HeightField* newHF = HeightFieldUtils::createSubSample(
                    ancestorHF.get(),
                    ancestorKey.getGeoExtent(),
                    key.getGeoExtent());

                newHFLayer = new osgTerrain::HeightFieldLayer( newHF );
                newHFLayer->setLocator( defaultLocator );
                tile->setElevationLayer( newHFLayer );                
                tile->setElevationLOD( ancestorTile->getElevationLOD() );
            }
        }
    }

    if ( !newHFLayer )
    {
        newHFLayer = new osgTerrain::HeightFieldLayer();
        newHFLayer->setHeightField( createEmptyHeightField( key, 8, 8 ) );
        newHFLayer->setLocator( defaultLocator );
        tile->setElevationLOD( -1 );
    }

    if ( newHFLayer )
    {
        tile->setElevationLayer( newHFLayer );
    }
}


osgTerrain::HeightFieldLayer*
OSGTileFactory::createPlaceholderHeightfieldLayer(osg::HeightField* ancestorHF,
                                                  const TileKey& ancestorKey,
                                                  const TileKey& key,
                                                  GeoLocator* keyLocator )
{
    osgTerrain::HeightFieldLayer* hfLayer = NULL;

    osg::HeightField* newHF = HeightFieldUtils::createSubSample(
        ancestorHF,
        ancestorKey.getGeoExtent(),
        key.getGeoExtent() );

    newHF->setSkirtHeight( ancestorHF->getSkirtHeight() / 2.0 );

    hfLayer = new osgTerrain::HeightFieldLayer( newHF );
    hfLayer->setLocator( keyLocator );

    return hfLayer;
}

osg::Node*
OSGTileFactory::createTile( Map* map, CustomTerrain* terrain, const TileKey& key, bool populateLayers, bool wrapInPagedLOD, bool fallback, bool &validData )
{
    if ( populateLayers )
    {        
        return createPopulatedTile( map, terrain, key, wrapInPagedLOD, fallback, validData);
    }
    else
    {
        //Placeholders always contain valid data
        validData = true;
        return createPlaceholderTile( map, terrain, key);
    }
}



osg::Node*
OSGTileFactory::createPlaceholderTile( Map* map, CustomTerrain* terrain, const TileKey& key )
{
    // Start out by finding the nearest registered ancestor tile, since the placeholder is
    // going to be based on inherited data. Note- the ancestor may not be the immediate
    // parent, b/c the parent may or may not be in the scene graph.
    TileKey ancestorKey = key.createParentKey();
    osg::ref_ptr<CustomTile> ancestorTile;
    while( !ancestorTile.valid() && ancestorKey.valid() )
    {
        terrain->getCustomTile( ancestorKey.getTileId(), ancestorTile );
        if ( !ancestorTile.valid() )
            ancestorKey = ancestorKey.createParentKey();
    }
    if ( !ancestorTile.valid() )
    {
        OE_WARN << LC << "cannot find ancestor tile for (" << key.str() << ")" <<std::endl;
        return 0L;
    }

    OE_DEBUG << LC << "Creating placeholder for " << key.str() << std::endl;
    Threading::ScopedReadLock lock( map->getMapDataMutex() );

    bool isProjected = map->getCoordinateSystemType() == Map::CSTYPE_PROJECTED;
    bool isPlateCarre = isProjected && map->getProfile()->getSRS()->isGeographic();
    bool isGeocentric = !isProjected;

    const MapLayerList& imageMapLayers = map->getImageMapLayers();
    const MapLayerList& hfMapLayers = map->getHeightFieldMapLayers();

    bool hasElevation = hfMapLayers.size() > 0;

    // Build a "placeholder" tile.
    double xmin, ymin, xmax, ymax;
    key.getGeoExtent().getBounds( xmin, ymin, xmax, ymax );

    // A locator will place the tile on the globe:
    osg::ref_ptr<GeoLocator> locator = GeoLocator::createForKey( key, map );

    // The empty tile:
    CustomTile* tile = new CustomTile( key, locator.get() );
    tile->setTerrainTechnique( osg::clone(terrain->getTerrainTechniquePrototype(), osg::CopyOp::DEEP_COPY_ALL) );
    tile->setVerticalScale( _terrainOptions.verticalScale().value() );
    tile->setRequiresNormals( true );
    tile->setDataVariance( osg::Object::DYNAMIC );
    tile->setLocator( locator.get() );

    // Attach an updatecallback to normalize the edges of TerrainTiles.
    if ( hasElevation && _terrainOptions.normalizeEdges().get() )
    {
        tile->setUpdateCallback(new TerrainTileEdgeNormalizerUpdateCallback());
        tile->setDataVariance(osg::Object::DYNAMIC);
    }

    // Generate placeholder imagery and elevation layers. These "inherit" data from an
    // ancestor tile.
    {
        Threading::ScopedReadLock parentLock( ancestorTile->getTileLayersMutex() );
        addPlaceholderImageLayers( tile, ancestorTile.get(), imageMapLayers, locator.get(), key );
        addPlaceholderHeightfieldLayer( tile, ancestorTile.get(), locator.get(), key, ancestorKey );
    }

    // calculate the switching distances:
    osg::BoundingSphere bs = tile->getBound();
    double max_range = 1e10;
    double radius = bs.radius();
    double min_range = radius * _terrainOptions.minTileRangeFactor().get();

    // Set the skirt height of the heightfield
    osgTerrain::HeightFieldLayer* hfLayer = static_cast<osgTerrain::HeightFieldLayer*>(tile->getElevationLayer());
    if (!hfLayer)
    {
        OE_WARN << LC << "Warning: Couldn't get hfLayer for " << key.str() << std::endl;
    }
    hfLayer->getHeightField()->setSkirtHeight(radius * _terrainOptions.heightFieldSkirtRatio().get() );

    // In a Plate Carre tesselation, scale the heightfield elevations from meters to degrees
    if ( isPlateCarre && hfLayer->getHeightField() )
        HeightFieldUtils::scaleHeightFieldToDegrees( hfLayer->getHeightField() );

    bool markTileLoaded = false;

    if ( _terrainOptions.loadingPolicy()->mode().get() != LoadingPolicy::MODE_STANDARD )
    {
        markTileLoaded = true;
        tile->setUseLayerRequests( true );
        tile->setHasElevationHint( hasElevation );
    }

    // install a tile switcher:
    tile->setTerrainRevision( terrain->getRevision() );
    tile->setTerrain( terrain );
    terrain->registerTile( tile );

    osg::Node* result = 0L;


    // create a PLOD so we can keep subdividing:
    osg::PagedLOD* plod = new osg::PagedLOD();
    plod->setCenter( bs.center() );
    plod->addChild( tile, min_range, max_range );

    if ( key.getLevelOfDetail() < getTerrainOptions().maxLOD().get() )
    {
        plod->setFileName( 1, createURI( _engineId, key ) ); //map->getId(), key ) );
        plod->setRange( 1, 0.0, min_range );
    }
    else
    {
        plod->setRange( 0, 0, FLT_MAX );
    }

#if USE_FILELOCATIONCALLBACK
    osgDB::Options* options = new osgDB::Options;
    options->setFileLocationCallback( new FileLocationCallback);
    plod->setDatabaseOptions( options );
#endif

    result = plod;

    // Install a callback that will load the actual tile data via the pager.
    result->addCullCallback( new TileImageBackfillCallback() );

    // Install a cluster culler (FIXME for cube mode)
    bool isCube = map->getCoordinateSystemType() == Map::CSTYPE_GEOCENTRIC_CUBE;
    if ( isGeocentric && !isCube )
    {
        osg::ClusterCullingCallback* ccc = createClusterCullingCallback( tile, locator->getEllipsoidModel() );
        result->addCullCallback( ccc );
    }     


    //result = plod;

    return result;
}

osg::Node*
OSGTileFactory::createPopulatedTile( Map* map, CustomTerrain* terrain, const TileKey& key, bool wrapInPagedLOD, bool fallback, bool &validData )
{
    Threading::ScopedReadLock lock( map->getMapDataMutex() );

    bool isProjected = map->getCoordinateSystemType() == Map::CSTYPE_PROJECTED;
    bool isPlateCarre = isProjected && map->getProfile()->getSRS()->isGeographic();
    bool isGeocentric = !isProjected;

    double xmin, ymin, xmax, ymax;
    key.getGeoExtent().getBounds( xmin, ymin, xmax, ymax );

    GeoImageList image_tiles;

    const MapLayerList& imageMapLayers = map->getImageMapLayers();
    const MapLayerList& hfMapLayers = map->getHeightFieldMapLayers();

    // Collect the image layers
    bool empty_map = imageMapLayers.size() == 0 && hfMapLayers.size() == 0;

    // Whether to use a special mercator locator instead of reprojecting data to spherical mercator
    bool useMercatorLocator = true;

    // Create the images for the tile
    for( MapLayerList::const_iterator i = imageMapLayers.begin(); i != imageMapLayers.end(); i++ )
    {
        MapLayer* layer = i->get();

        osg::ref_ptr<GeoImage> image;
        //Only create images if the key is valid
        if ( layer->isKeyValid( key ) )
        {
            if ( _L2cache )
                image = _L2cache->createImage( layer, key );
            else
                image = layer->createImage( key );
        }
        image_tiles.push_back(image.get());

        // if any one of the layers explicity disables the merc fast path, disable for the whole thing:
        if ( layer->useMercatorFastPath().isSetTo( false ) )
            useMercatorLocator = false;
    }

    bool hasElevation = false;

    //Create the heightfield for the tile
    osg::ref_ptr<osg::HeightField> hf;
    if ( hfMapLayers.size() > 0 )
    {
        hf = map->createHeightField( key, false, _terrainOptions.elevationInterpolation().value());     
    }

    //If we are on the first LOD and we couldn't get a heightfield tile, just create an empty one.  Otherwise you can run into the situation
    //where you could have an inset heightfield on one hemisphere and the whole other hemisphere won't show up.
    if (map->isGeocentric() && key.getLevelOfDetail() <= 1 && !hf.valid())
    {
        hf = createEmptyHeightField( key );
    }
    hasElevation = hf.valid();

    //Determine if we've created any images
    unsigned int numValidImages = 0;
    for (unsigned int i = 0; i < image_tiles.size(); ++i)
    {
        if (image_tiles[i].valid()) numValidImages++;
    }


    //If we couldn't create any imagery or heightfields, bail out
    if (!hf.valid() && (numValidImages == 0) && !empty_map)
    {
        OE_DEBUG << LC << "Could not create any imagery or heightfields for " << key.str() <<".  Not building tile" << std::endl;
        validData = false;

        //If we're not asked to fallback on previous LOD's and we have no data, return NULL
        if (!fallback)
        {
            return NULL;
        }
    }
    else
    {
        validData = true;
    }

    //Try to interpolate any missing image layers from parent tiles
    for (unsigned int i = 0; i < imageMapLayers.size(); i++ )
    {
        if (!image_tiles[i].valid())
        {
            GeoImage* image = NULL;
            if (imageMapLayers[i]->isKeyValid(key))
            {
                //If the key was valid and we have no image, then something possibly went wrong with the image creation such as a server being busy.
                image = createValidGeoImage(imageMapLayers[i].get(), key);
            }

            //If we still couldn't create an image, either something is really wrong or the key wasn't valid, so just create a transparent placeholder image
            if (!image)
            {
                //If the image is not valid, create an empty texture as a placeholder
                image = new GeoImage(ImageUtils::createEmptyImage(), key.getGeoExtent());
            }

            //Assign the new image to the proper place in the list
            image_tiles[i] = image;
        }
    }

    //Fill in missing heightfield information from parent tiles
    if (!hf.valid())
    {
        //We have no heightfield sources, 
        if ( hfMapLayers.size() == 0 )
        {
            hf = createEmptyHeightField( key );
        }
        else
        {
            //Try to get a heightfield again, but this time fallback on parent tiles
            hf = map->createHeightField( key, true, _terrainOptions.elevationInterpolation().value());
            if (!hf.valid())
            {
                //We couldn't get any heightfield, so just create an empty one.
                hf = createEmptyHeightField( key );
            }
            else
            {
                hasElevation = true;
            }
        }
    }


    // In a Plate Carre tesselation, scale the heightfield elevations from meters to degrees
    if ( isPlateCarre )
    {
        HeightFieldUtils::scaleHeightFieldToDegrees( hf.get() );
    }

    osg::ref_ptr<GeoLocator> locator = GeoLocator::createForKey( key, map );
    osgTerrain::HeightFieldLayer* hf_layer = new osgTerrain::HeightFieldLayer();
    hf_layer->setLocator( locator.get() );
    hf_layer->setHeightField( hf.get() );

    CustomTile* tile = new CustomTile( key, locator.get() );
    tile->setTerrainTechnique( osg::clone(terrain->getTerrainTechniquePrototype(), osg::CopyOp::DEEP_COPY_ALL) );
    tile->setVerticalScale( _terrainOptions.verticalScale().value() );
    tile->setLocator( locator.get() );
    tile->setElevationLayer( hf_layer );
    tile->setRequiresNormals( true );
    tile->setDataVariance(osg::Object::DYNAMIC);

    //Attach an updatecallback to normalize the edges of TerrainTiles.
    if (hasElevation && _terrainOptions.normalizeEdges().get() )
    {
        tile->setUpdateCallback(new TerrainTileEdgeNormalizerUpdateCallback());
        tile->setDataVariance(osg::Object::DYNAMIC);
    }

    //Assign the terrain system to the TerrainTile.
    //It is very important the terrain system is set while the MapConfig's sourceMutex is locked.
    //This registers the terrain tile so that adding/removing layers are always in sync.  If you don't do this
    //you can end up with a situation where the database pager is waiting to merge a tile, then a layer is added, then
    //the tile is finally merged and is out of sync.

    double min_units_per_pixel = DBL_MAX;

    int layer = 0;
#if 0
    // create contour layer:
    if (map->getContourTransferFunction() != NULL)
    {
        osgTerrain::ContourLayer* contourLayer(new osgTerrain::ContourLayer(map->getContourTransferFunction()));

        contourLayer->setMagFilter(_terrainOptions.getContourMagFilter().value());
        contourLayer->setMinFilter(_terrainOptions.getContourMinFilter().value());
        tile->setColorLayer(layer,contourLayer);
        ++layer;
    }
#endif

    for (unsigned int i = 0; i < image_tiles.size(); ++i)
    {
        if (image_tiles[i].valid())
        {
            double img_xmin, img_ymin, img_xmax, img_ymax;

            //Specify a new locator for the color with the coordinates of the TileKey that was actually used to create the image
            osg::ref_ptr<GeoLocator> img_locator;

            GeoImage* geo_image = image_tiles[i].get();

            // Use a special locator for mercator images (instead of reprojecting)
            if (map->getProfile()->getSRS()->isGeographic() &&
                geo_image->getSRS()->isMercator() && 
                useMercatorLocator )
            {
                GeoExtent geog_ext = image_tiles[i]->getExtent().transform(image_tiles[i]->getExtent().getSRS()->getGeographicSRS());
                geog_ext.getBounds( img_xmin, img_ymin, img_xmax, img_ymax );
                img_locator = key.getProfile()->getSRS()->createLocator( img_xmin, img_ymin, img_xmax, img_ymax );
                img_locator = new MercatorLocator( *img_locator.get(), geo_image->getExtent() );
            }
            else
            {
                image_tiles[i]->getExtent().getBounds( img_xmin, img_ymin, img_xmax, img_ymax );

                img_locator = key.getProfile()->getSRS()->createLocator( 
                    img_xmin, img_ymin, img_xmax, img_ymax, isPlateCarre );
            }

            if ( isGeocentric )
                img_locator->setCoordinateSystemType( osgTerrain::Locator::GEOCENTRIC );

            TransparentLayer* img_layer = new TransparentLayer(geo_image->getImage(), imageMapLayers[i].get());
            img_layer->setLevelOfDetail( key.getLevelOfDetail() );
            img_layer->setName( imageMapLayers[i]->getName() );
            img_layer->setLocator( img_locator.get());
            img_layer->setMinFilter( imageMapLayers[i]->getMinFilter().value());
            img_layer->setMagFilter( imageMapLayers[i]->getMagFilter().value());

            double upp = geo_image->getUnitsPerPixel();

            // Scale the units per pixel to degrees if the image is mercator (and the key is geo)
            if ( geo_image->getSRS()->isMercator() && key.getGeoExtent().getSRS()->isGeographic() )
                upp *= 1.0f/111319.0f;

            min_units_per_pixel = osg::minimum(upp, min_units_per_pixel);

            tile->setColorLayer( layer, img_layer );
            layer++;
        }
    }

    osg::BoundingSphere bs = tile->getBound();
    double max_range = 1e10;
    double radius = bs.radius();

#if 1
    double min_range = radius * _terrainOptions.minTileRangeFactor().get();
    osg::LOD::RangeMode mode = osg::LOD::DISTANCE_FROM_EYE_POINT;
#else
    double width = key.getGeoExtent().width();	
    if (min_units_per_pixel == DBL_MAX) min_units_per_pixel = width/256.0;
    double min_range = (width / min_units_per_pixel) * _terrainOptions.getMinTileRangeFactor(); 
    osg::LOD::RangeMode mode = osg::LOD::PIXEL_SIZE_ON_SCREEN;
#endif


    // a skirt hides cracks when transitioning between LODs:
    hf->setSkirtHeight(radius * _terrainOptions.heightFieldSkirtRatio().get() );

    // for now, cluster culling does not work for CUBE rendering
    bool isCube = map->getCoordinateSystemType() == Map::CSTYPE_GEOCENTRIC_CUBE;
    if ( isGeocentric && !isCube )
    {
        //TODO:  Work on cluster culling computation for cube faces
        osg::ClusterCullingCallback* ccc = createClusterCullingCallback(tile, locator->getEllipsoidModel() );
        tile->setCullCallback( ccc );
    }

    // Wait until now, when the tile is fully baked, to assign the terrain to the tile.
    // Placeholder tiles might try to locate this tile as an ancestor, and access its layers
    // and locators...so they must be intact before making this tile available via setTerrain.
    //
    // If there's already a placeholder tile registered, this will be ignored. If there isn't,
    // this will register the new tile.
    tile->setTerrain( terrain );
    terrain->registerTile( tile );

    // Set the tile's revision to the current terrain revision
    tile->setTerrainRevision( static_cast<CustomTerrain*>(terrain)->getRevision() );

    if ( _terrainOptions.loadingPolicy()->mode() != LoadingPolicy::MODE_STANDARD && key.getLevelOfDetail())
    {
        tile->setUseLayerRequests( true );
        tile->setHasElevationHint( hasElevation );
    }

    tile->setTerrainRevision( terrain->getRevision() );
    tile->setDataVariance( osg::Object::DYNAMIC );

    osg::Node* result = 0L;

    if (wrapInPagedLOD)
    {
        // create a PLOD so we can keep subdividing:
        osg::PagedLOD* plod = new osg::PagedLOD();
        plod->setCenter( bs.center() );
        plod->addChild( tile, min_range, max_range );

        std::string filename = createURI( _engineId, key ); //map->getId(), key );

        //Only add the next tile if it hasn't been blacklisted
        bool isBlacklisted = osgEarth::Registry::instance()->isBlacklisted( filename );
        if (!isBlacklisted && key.getLevelOfDetail() < this->getTerrainOptions().maxLOD().value() && validData )
        {
            plod->setFileName( 1, filename  );
            plod->setRange( 1, 0.0, min_range );
        }
        else
        {
            plod->setRange( 0, 0, FLT_MAX );
        }

#if USE_FILELOCATIONCALLBACK
        osgDB::Options* options = new osgDB::Options;
        options->setFileLocationCallback( new FileLocationCallback() );
        plod->setDatabaseOptions( options );
#endif
        result = plod;

        if ( tile->getUseLayerRequests() )
            result->addCullCallback( new TileImageBackfillCallback() );
    }
    else
    {
        result = tile;
    }

    return result;
}


osgTerrain::ImageLayer* 
OSGTileFactory::createImageLayer(Map* map, 
                                 MapLayer* layer,
                                 const TileKey& key,
                                 ProgressCallback* progress)
{
    Threading::ScopedReadLock lock( map->getMapDataMutex() );

    osg::ref_ptr< GeoImage > geoImage;

    //If the key is valid, try to get the image from the MapLayer
    if (layer->isKeyValid( key ) )
    {
        geoImage = layer->createImage(key, progress);
    }
    else
    {
        //If the key is not valid, simply make a transparent tile
        geoImage = new GeoImage(ImageUtils::createEmptyImage(), key.getGeoExtent());
    }

    if (geoImage.valid())
    {
        bool isProjected = map->getCoordinateSystemType() == Map::CSTYPE_PROJECTED;
        bool isPlateCarre = isProjected && map->getProfile()->getSRS()->isGeographic();
        bool isGeocentric = !isProjected;

        osg::ref_ptr<GeoLocator> imgLocator;

        // Use a special locator for mercator images (instead of reprojecting)
        if (map->getProfile()->getSRS()->isGeographic() &&
            geoImage->getSRS()->isMercator() &&
            layer->useMercatorFastPath() == true )
        {
            GeoExtent gx = geoImage->getExtent().transform( geoImage->getExtent().getSRS()->getGeographicSRS() );
            imgLocator = key.getProfile()->getSRS()->createLocator( gx.xMin(), gx.yMin(), gx.xMax(), gx.yMax() );
            imgLocator = new MercatorLocator( *imgLocator.get(), geoImage->getExtent() );
        }
        else
        {
            const GeoExtent& gx = geoImage->getExtent();
            imgLocator = GeoLocator::createForKey( key, map );
        }

        if ( isGeocentric )
            imgLocator->setCoordinateSystemType( osgTerrain::Locator::GEOCENTRIC );

        //osgTerrain::ImageLayer* imgLayer = new osgTerrain::ImageLayer( geoImage->getImage() );
        TransparentLayer* imgLayer = new TransparentLayer(geoImage->getImage(), layer);
        imgLayer->setLocator( imgLocator.get() );
        imgLayer->setLevelOfDetail( key.getLevelOfDetail() );
        imgLayer->setMinFilter( layer->getMinFilter().value());
        imgLayer->setMagFilter( layer->getMagFilter().value());
        return imgLayer;
    }
    return NULL;
}

osgTerrain::HeightFieldLayer* 
OSGTileFactory::createHeightFieldLayer( Map* map, const TileKey& key, bool exactOnly )
{
    Threading::ScopedReadLock lock( map->getMapDataMutex() );

    bool isProjected = map->getCoordinateSystemType() == Map::CSTYPE_PROJECTED;
    bool isPlateCarre = isProjected && map->getProfile()->getSRS()->isGeographic();

    // try to create a heightfield at native res:
    osg::ref_ptr<osg::HeightField> hf = map->createHeightField( key, !exactOnly, _terrainOptions.elevationInterpolation().value());
    if ( !hf )
    {
        if ( exactOnly )
            return NULL;
        else
            hf = createEmptyHeightField( key );
    }

    // In a Plate Carre tesselation, scale the heightfield elevations from meters to degrees
    if ( isPlateCarre )
    {
        HeightFieldUtils::scaleHeightFieldToDegrees( hf );
    }

    osgTerrain::HeightFieldLayer* hfLayer = new osgTerrain::HeightFieldLayer( hf.get() );

    GeoLocator* locator = GeoLocator::createForKey( key, map );
    hfLayer->setLocator( locator );

    return hfLayer;
}

osg::ClusterCullingCallback*
OSGTileFactory::createClusterCullingCallback(osgTerrain::TerrainTile* tile, osg::EllipsoidModel* et)
{
    //This code is a very slightly modified version of the DestinationTile::createClusterCullingCallback in VirtualPlanetBuilder.
    osg::HeightField* grid = ((osgTerrain::HeightFieldLayer*)tile->getElevationLayer())->getHeightField();
    if (!grid) return 0;

    float verticalScale = 1.0f;
    CustomTile* customTile = dynamic_cast<CustomTile*>(tile);
    if (customTile)
    {
        verticalScale = customTile->getVerticalScale();
    }

    double globe_radius = et ? et->getRadiusPolar() : 1.0;
    unsigned int numColumns = grid->getNumColumns();
    unsigned int numRows = grid->getNumRows();

    double midLong = grid->getOrigin().x()+grid->getXInterval()*((double)(numColumns-1))*0.5;
    double midLat = grid->getOrigin().y()+grid->getYInterval()*((double)(numRows-1))*0.5;
    double midZ = grid->getOrigin().z();

    double midX,midY;
    et->convertLatLongHeightToXYZ(osg::DegreesToRadians(midLat),osg::DegreesToRadians(midLong),midZ, midX,midY,midZ);

    osg::Vec3 center_position(midX,midY,midZ);

    osg::Vec3 center_normal(midX,midY,midZ);
    center_normal.normalize();

    osg::Vec3 transformed_center_normal = center_normal;

    unsigned int r,c;

    // populate the vertex/normal/texcoord arrays from the grid.
    double orig_X = grid->getOrigin().x();
    double delta_X = grid->getXInterval();
    double orig_Y = grid->getOrigin().y();
    double delta_Y = grid->getYInterval();
    double orig_Z = grid->getOrigin().z();


    float min_dot_product = 1.0f;
    float max_cluster_culling_height = 0.0f;
    float max_cluster_culling_radius = 0.0f;

    for(r=0;r<numRows;++r)
    {
        for(c=0;c<numColumns;++c)
        {
            double X = orig_X + delta_X*(double)c;
            double Y = orig_Y + delta_Y*(double)r;
            double Z = orig_Z + grid->getHeight(c,r) * verticalScale;
            double height = Z;

            et->convertLatLongHeightToXYZ(
                osg::DegreesToRadians(Y), osg::DegreesToRadians(X), Z,
                X, Y, Z);

            osg::Vec3d v(X,Y,Z);
            osg::Vec3 dv = v - center_position;
            double d = sqrt(dv.x()*dv.x() + dv.y()*dv.y() + dv.z()*dv.z());
            double theta = acos( globe_radius/ (globe_radius + fabs(height)) );
            double phi = 2.0 * asin (d*0.5/globe_radius); // d/globe_radius;
            double beta = theta+phi;
            double cutoff = osg::PI_2 - 0.1;

            //log(osg::INFO,"theta="<<theta<<"\tphi="<<phi<<" beta "<<beta);
            if (phi<cutoff && beta<cutoff)
            {
                float local_dot_product = -sin(theta + phi);
                float local_m = globe_radius*( 1.0/ cos(theta+phi) - 1.0);
                float local_radius = static_cast<float>(globe_radius * tan(beta)); // beta*globe_radius;
                min_dot_product = osg::minimum(min_dot_product, local_dot_product);
                max_cluster_culling_height = osg::maximum(max_cluster_culling_height,local_m);      
                max_cluster_culling_radius = osg::maximum(max_cluster_culling_radius,local_radius);
            }
            else
            {
                //log(osg::INFO,"Turning off cluster culling for wrap around tile.");
                return 0;
            }
        }
    }    

    osg::ClusterCullingCallback* ccc = new osg::ClusterCullingCallback;

    ccc->set(center_position + transformed_center_normal*max_cluster_culling_height ,
        transformed_center_normal, 
        min_dot_product,
        max_cluster_culling_radius);

    return ccc;
}