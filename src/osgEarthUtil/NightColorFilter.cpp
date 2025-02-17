/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2014 Pelican Mapping
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
*
*/
#include <osgEarthUtil/NightColorFilter>
#include <osgEarth/VirtualProgram>
#include <osgEarth/StringUtils>
#include <osgEarth/ThreadingUtils>
#include <osg/Program>
#include <OpenThreads/Atomic>

using namespace osgEarth;
using namespace osgEarth::Util;

namespace
{
    static OpenThreads::Atomic s_uniformNameGen;

    static const char* s_commonShaderSource =

        "#version 110 \n"
        "}\n";


    static const char* s_localShaderSource =
        "#version 110\n"

        "varying vec3 atmos_lightDir;\n"    // light direction (view coords)
        "varying vec3 atmos_up;\n"          // earth up vector at fragment (in view coords)

        "void __ENTRY_POINT__(inout vec4 color)\n"
        "{\n"
        "    vec3 L = normalize(atmos_lightDir);\n"
        "    vec3 N = normalize(atmos_up);\n"
        "    float NdotL = dot(N,L);\n"
        "    float vmin = -0.25;\n"
        "    float vmax = 0.0;\n"
        //   Remap the -0.25 to 0 to 0 to 1.0
        "    float day = (clamp( NdotL, vmin, vmax) - vmin)/(vmax-vmin);\n"
        "    color.a *= (1.0 - day);\n"
        "} \n";
}

//---------------------------------------------------------------------------

#define FUNCTION_PREFIX "osgearthutil_nightColorFilter_"
#define UNIFORM_PREFIX  "osgearthutil_u_night_"

//---------------------------------------------------------------------------

NightColorFilter::NightColorFilter(void)
{
    init();
}

void NightColorFilter::init()
{
    // Generate a unique name for this filter's uniform. This is necessary
    // so that each layer can have a unique uniform and entry point.
    m_instanceId = (++s_uniformNameGen) - 1;
}

std::string NightColorFilter::getEntryPointFunctionName(void) const
{
    return (osgEarth::Stringify() << FUNCTION_PREFIX << m_instanceId);
}

void NightColorFilter::install(osg::StateSet* stateSet) const
{
    // safe: will not add twice.
    osgEarth::VirtualProgram* vp = dynamic_cast<osgEarth::VirtualProgram*>(stateSet->getAttribute(VirtualProgram::SA_TYPE));
    if (vp)
    {
        // build the local shader (unique per instance). We will
        // use a template with search and replace for this one.
        std::string entryPoint = osgEarth::Stringify() << FUNCTION_PREFIX << m_instanceId;
        std::string code = s_localShaderSource;
        osgEarth::replaceIn(code, "__ENTRY_POINT__", entryPoint);

        osg::Shader* main = new osg::Shader(osg::Shader::FRAGMENT, code);
        //main->setName(entryPoint);
        vp->setShader(entryPoint, main);
    }
}


//---------------------------------------------------------------------------

OSGEARTH_REGISTER_COLORFILTER( night, osgEarth::Util::NightColorFilter );


NightColorFilter::NightColorFilter(const Config& conf)
{
    init();
}

Config
NightColorFilter::getConfig() const
{
    Config conf("night");   
    return conf;
}
