/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2012 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include "LandCoverTerrainEffect"
#include "SplatOptions"
#include "NoiseTextureFactory"

#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osgEarth/VirtualProgram>
#include <osgEarth/TerrainEngineNode>
#include <osgEarth/URI>
#include <osgEarth/ShaderLoader>
#include <osgEarth/ImageUtils>

#include <osg/Texture2D>
#include <osg/BlendFunc>
#include <osg/Multisample>
#include <osgUtil/Optimizer>

#include "SplatShaders"

#define LC "[LandCoverTerrainEffect] "

using namespace osgEarth;
using namespace osgEarth::Splat;

LandCoverTerrainEffect::LandCoverTerrainEffect() :
_noiseTexUnit    ( -1 ),
_landCoverTexUnit( -1 )
{
    //nop
}

void
LandCoverTerrainEffect::setDBOptions(const osgDB::Options* dbo)
{
    _dbo = dbo;
}

void
LandCoverTerrainEffect::onInstall(TerrainEngineNode* engine)
{
    if ( !engine )
        return;

    if ( engine )
    {
        // first make sure there is land cover data available:
        bool landCoverActive = false;
        for(Zones::const_iterator z = _zones.begin(); z != _zones.end(); ++z)
        {
            Zone* zone = z->get();
            LandCover* landcover = zone->getLandCover();
            if ( landcover )
            {
                landCoverActive = true;
            }
        }

        if ( landCoverActive )
        {
            // install a noise texture if we haven't already.
            osg::StateSet* terrainStateSet = engine->getOrCreateStateSet();

            // check whether it's already installed by another extension:
            if ( terrainStateSet->getUniform("oe_splat_noiseTex") == 0L )
            {
                // reserve a texture unit:
                if (engine->getResources()->reserveTextureImageUnit(_noiseTexUnit, "Splat Noise"))
                {
                    NoiseTextureFactory noise;
                    terrainStateSet->setTextureAttribute( _noiseTexUnit, noise.create(256u, 4u) );
                    terrainStateSet->addUniform( new osg::Uniform("oe_splat_noiseTex", _noiseTexUnit) );
                }
                else
                {
                    OE_WARN << LC << "No texture image unit available for LandCover Noise. Aborting.\n";
                    return;
                }
            }
            
            int _landCoverTexUnit;
            if ( !engine->getResources()->reserveTextureImageUnit(_landCoverTexUnit, "LandCover") )
            {
                OE_WARN << LC << "No texture image unit available for LandCover." << std::endl;
                return;
            }

            // The patch callback that will render the land cover:
            _tilePatchCallback = new LandCoverTilePatchCallback();
            engine->addTilePatchCallback( _tilePatchCallback.get() );

            for(Zones::iterator z = _zones.begin(); z != _zones.end(); ++z)
            {
                Zone* zone = z->get();
                LandCover* landCover = zone->getLandCover();
                if ( landCover )
                {
                    _tilePatchCallback->_zones.push_back(LandCoverTilePatchCallback::ZoneData());
                    LandCoverTilePatchCallback::ZoneData& zoneData = _tilePatchCallback->_zones.back();

                    for(LandCoverLayers::const_iterator i = landCover->getLayers().begin();
                        i != landCover->getLayers().end();
                        ++i)
                    {
                        const LandCoverLayer* layer = i->get();
                        if ( layer )
                        {
                            if ( !layer->getBiomes().empty() || layer->getTotalNumBillboards() > 0 )
                            {
                                zoneData._layers.push_back(LandCoverTilePatchCallback::LayerData());
                                LandCoverTilePatchCallback::LayerData& layerData = zoneData._layers.back();
                                
                                layerData._layer = layer;

                                osg::StateSet* stateset = new osg::StateSet();
                                layerData._stateSet = stateset;

                                bool useMask = (landCover->getMaskLayer() != 0L);

                                // Install the land cover shaders on the state set
                                VirtualProgram* vp = VirtualProgram::getOrCreate(stateset);
                                vp->setName("Land Cover (" + layer->getName() + ")");
                                LandCoverShaders shaders;
                                if ( useMask )
                                {
                                    shaders.replace("MASK_SAMPLER", landCover->getMaskLayer()->shareTexUniformName().get());
                                    shaders.replace("MASK_TEXTURE", landCover->getMaskLayer()->shareTexMatUniformName().get());
                                }
                                shaders.loadAll( vp, _dbo.get() );

                                // Generate the coverage acceptor shader
                                osg::Shader* covTest = layer->createPredicateShader( getCoverage() );
                                //covTest->setType( osg::Shader::TESSCONTROL );
                                covTest->setType( osg::Shader::GEOMETRY );
                                vp->setShader( covTest );

                                osg::ref_ptr<osg::Shader> layerShader = layer->createShader();
                                layerShader->setType( osg::Shader::GEOMETRY );
                                vp->setShader( layerShader );

                                OE_INFO << LC << "Adding land cover layer: " << layer->getName() << " to Zone " << zone->getName() << " at LOD " << layer->getLOD() << "\n";

                                // Install the uniforms
                                stateset->addUniform( new osg::Uniform("oe_landcover_windFactor", layer->getWind()) );
                                stateset->addUniform( new osg::Uniform("oe_landcover_noise", 1.0f) );
                                stateset->addUniform( new osg::Uniform("oe_landcover_ao", 0.5f) );
                                stateset->addUniform( new osg::Uniform("oe_landcover_exposure", 1.0f) );
                    
                                stateset->addUniform( new osg::Uniform("oe_landcover_density",     layer->getDensity()) );
                                stateset->addUniform( new osg::Uniform("oe_landcover_fill",        layer->getFill()) );
                                stateset->addUniform( new osg::Uniform("oe_landcover_maxDistance", layer->getMaxDistance()) );

                                stateset->addUniform( new osg::Uniform("oe_landcover_brightness",  layer->getBrightness()) );
                                stateset->addUniform( new osg::Uniform("oe_landcover_contrast",    layer->getContrast()) );

                                stateset->addUniform( new osg::Uniform("oe_landcover_useMask", useMask) );

                                // enable alpha-to-coverage multisampling for vegetation.
                                stateset->setMode(GL_SAMPLE_ALPHA_TO_COVERAGE_ARB, 1);

                                // uniform that communicates the availability of multisampling.
                                stateset->addUniform( new osg::Uniform(
                                    "oe_terrain_hasMultiSamples",
                                    osg::DisplaySettings::instance()->getMultiSamples()) );

                                stateset->setAttributeAndModes(
                                    new osg::BlendFunc(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO),
                                    osg::StateAttribute::OVERRIDE );

                                stateset->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);

                                // Build the texture array!
                                int s=-1, t=-1;
                                osg::Texture2DArray* tex = new osg::Texture2DArray();
                            
                                int arrayIndex = 0;
                                for(int b=0; b<layer->getBiomes().size(); ++b)
                                {
                                    const LandCoverBiome* biome = layer->getBiomes().at(b);
                                
                                    for(int i=0; i<biome->getBillboards().size(); ++i, ++arrayIndex)
                                    {
                                        const LandCoverBillboard& bb = biome->getBillboards().at(i);

                                        osg::ref_ptr<osg::Image> im;

                                        if ( s < 0 )
                                        {
                                            s  = bb._image->s();
                                            t  = bb._image->t();
                                            im = bb._image.get();
                                            tex->setTextureSize(s, t, layer->getTotalNumBillboards());                              
                                        }
                                        else
                                        {
                                            if ( bb._image->s() != s || bb._image->t() != t )
                                            {
                                                ImageUtils::resizeImage( bb._image.get(), s, t, im );
                                            }
                                            else
                                            {
                                                im = bb._image.get();
                                            }
                                        }

                                        tex->setImage( arrayIndex, im.get() );
                                    }
                                }
                                    
                                tex->setFilter(tex->MIN_FILTER, tex->NEAREST_MIPMAP_LINEAR);
                                tex->setFilter(tex->MAG_FILTER, tex->LINEAR);
                                tex->setWrap  (tex->WRAP_S, tex->CLAMP_TO_EDGE);
                                tex->setWrap  (tex->WRAP_T, tex->CLAMP_TO_EDGE);
                                tex->setUnRefImageDataAfterApply( true );
                                tex->setMaxAnisotropy( 4.0 );
                                tex->setResizeNonPowerOfTwoHint( false );

                                stateset->setTextureAttribute(_landCoverTexUnit, tex);
                                stateset->addUniform(new osg::Uniform("oe_landcover_texArray", _landCoverTexUnit) );
                            }
                            else
                            {
                                OE_WARN << LC << "ILLEGAL: land cover layer with no biomes or no billboards defined\n";
                            }
                        }
                        else
                        {
                            OE_WARN << LC << "ILLEGAL: empty layer found in land cover layer list\n";
                        }
                    }
                }
                else
                {
                    // not an error.
                    OE_DEBUG << LC << "zone contains no land cover information\n";
                }
            }
        }
        else
        {
            OE_DEBUG << LC << "No land cover information found\n";
        }
    }
}



void
LandCoverTerrainEffect::onUninstall(TerrainEngineNode* engine)
{
    if ( engine )
    {
        if ( _tilePatchCallback.valid() )
        {
            engine->removeTilePatchCallback( _tilePatchCallback.get() );

            if ( _landCoverTexUnit >= 0 )
                engine->getResources()->releaseTextureImageUnit( _landCoverTexUnit );

            if ( _noiseTexUnit >= 0 )
                engine->getResources()->releaseTextureImageUnit( _noiseTexUnit );
        }
    }
}
