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
 */
#include <osgEarth/VirtualProgram>

#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osgEarth/ShaderFactory>
#include <osgEarth/ShaderUtils>
#include <osgEarth/StringUtils>
#include <osgEarth/Containers>
#include <osg/Shader>
#include <osg/Program>
#include <osg/State>
#include <osg/Notify>
#include <osg/Version>
#include <osg/GL2Extensions>
#include <osg/GLExtensions>
#include <fstream>
#include <sstream>
#include <OpenThreads/Thread>

#define LC "[VirtualProgram] "

using namespace osgEarth;
using namespace osgEarth::ShaderComp;

#define OE_TEST OE_NULL
//#define OE_TEST OE_NOTICE
//#define USE_ATTRIB_ALIASES
//#define DEBUG_APPLY_COUNTS
//#define DEBUG_ACCUMULATION

#define USE_STACK_MEMORY 1

#define MAX_PROGRAM_CACHE_SIZE 128

#define MAKE_SHADER_ID(X) osgEarth::hashString( X )

//------------------------------------------------------------------------

namespace
{
    /** Locate a function by name in the location map. */
    bool findFunction(const std::string&               name, 
                      ShaderComp::FunctionLocationMap& flm, 
                      ShaderComp::Function**           output)
    {        
        for(ShaderComp::FunctionLocationMap::iterator i = flm.begin(); i != flm.end(); ++i )
        {
            ShaderComp::OrderedFunctionMap& ofm = i->second;
            for( ShaderComp::OrderedFunctionMap::iterator j = ofm.begin(); j != ofm.end(); ++j )
            {
                if ( j->second._name.compare(name) == 0 )
                {
                    (*output) = &j->second;
                    return true;
                }
            }
        }
        return false;
    }
}

//------------------------------------------------------------------------

// environment variable control
#define OSGEARTH_DUMP_SHADERS  "OSGEARTH_DUMP_SHADERS"
#define OSGEARTH_MERGE_SHADERS "OSGEARTH_MERGE_SHADERS"

namespace
{
#ifdef OSG_GLES2_AVAILABLE
    // GLES requires all shader code be merged into a since source
    bool s_mergeShaders = true;
#else
    bool s_mergeShaders = false;
#endif

    bool s_dumpShaders = false;        // debugging

    /** A device that lets us do a const search on the State's attribute map. OSG does not yet
        have a const way to do this. It has getAttributeVec() but that is non-const (it creates
        the vector if it doesn't exist); Newer versions have getAttributeMap(), but that does not
        go back to OSG 3.0. */
    struct StateEx : public osg::State 
    {
        static const VirtualProgram::AttrStack* getProgramStack(const osg::State& state)
        {            
            static osg::StateAttribute::TypeMemberPair pair(VirtualProgram::SA_TYPE, 0);
            const StateEx* sh = reinterpret_cast<const StateEx*>( &state );
            AttributeMap::const_iterator i = sh->_attributeMap.find( pair );
            return i != sh->_attributeMap.end() ? &(i->second.attributeVec) : 0L;
        }
    };

    typedef std::map<std::string, std::string> HeaderMap;

    // removes leading and trailing whitespace, and replaces all other
    // whitespace with single spaces
    std::string trimAndCompress(const std::string& in)
    {
        bool inwhite = true;
        std::stringstream buf;
        for( unsigned i=0; i<in.length(); ++i )
        {
            char c = in.at(i);
            if ( ::isspace(c) )
            {
                if ( !inwhite )
                {
                    buf << ' ';
                    inwhite = true;
                }
            }
            else
            {
                inwhite = false;
                buf << c;
            }
        }
        std::string r;
        r = buf.str();
        return trim(r);
    }


    void parseShaderForMerging( const std::string& source, unsigned& version, HeaderMap& headers, std::stringstream& body )
    {
        // break into lines:
        StringVector lines;
        StringTokenizer( source, lines, "\n", "", true, false );

        for( StringVector::const_iterator line_iter = lines.begin(); line_iter != lines.end(); ++line_iter )
        {
            std::string line = trimAndCompress(*line_iter);

            if ( line.size() > 0 )
            {
                StringVector tokens;
                StringTokenizer( line, tokens, " \t", "", false, true );

                if (tokens[0] == "#version")
                {
                    // find the highest version number.
                    if ( tokens.size() > 1 )
                    {
                        unsigned newVersion = osgEarth::as<unsigned>(tokens[1], 0);
                        if ( newVersion > version )
                        {
                            version = newVersion;
                        }
                    }
                }

                else if (
                    tokens[0] == "#extension"   ||
                    tokens[0] == "#define"      ||
                    tokens[0] == "precision"    ||
                    tokens[0] == "struct"       ||
                    tokens[0] == "varying"      ||
                    tokens[0] == "uniform"      ||
                    tokens[0] == "attribute")
                {
                    std::string& header = headers[line];
                    header = line;
                }

                else
                {
                    body << (*line_iter) << "\n";
                }
            }
        }
    }


    bool s_attribAliasSortFunc(const std::pair<std::string,std::string>& a, const std::pair<std::string,std::string>& b) {
        return a.first.size() > b.first.size();
    }


    /**
    * Replaces a shader's attribute values with their aliases
    */
    void applyAttributeAliases(
        osg::Shader*                             shader, 
        const VirtualProgram::AttribAliasVector& sortedAliases)
    {
        std::string src = shader->getShaderSource();
        for( VirtualProgram::AttribAliasVector::const_iterator i = sortedAliases.begin(); i != sortedAliases.end(); ++i )
        {
            //OE_DEBUG << LC << "Replacing " << i->first << " with " << i->second << std::endl;
            osgEarth::replaceIn( src, i->first, i->second );
        }
        shader->setShaderSource( src );
    }


    /**
    * Adds a new shader entry to the accumulated shader map, respecting the
    * override policy of both the existing entry (if there is one) and the 
    * new entry.
    */
    void addToAccumulatedMap(VirtualProgram::ShaderMap&         accumShaderMap,
                             const VirtualProgram::ShaderID&    shaderID,
                             const VirtualProgram::ShaderEntry& newEntry)
    {        

        // see if we're trying to disable a previous entry:
        if ((newEntry._overrideValue & osg::StateAttribute::ON) == 0 ) //TODO: check for higher override
        {
            // yes? remove it!
            accumShaderMap.erase( shaderID );
        }

        else
        {
            // see if there's a higher-up entry with the same ID:
            VirtualProgram::ShaderEntry& accumEntry = accumShaderMap[ shaderID ]; 

            // make sure we can add the new one:
            if ((accumEntry._shader.get() == 0L ) ||                                   // empty slot, fill it
                ((accumEntry._overrideValue & osg::StateAttribute::PROTECTED) != 0) || // new entry is protected
                ((accumEntry._overrideValue & osg::StateAttribute::OVERRIDE) == 0) )   // old entry does NOT override
            {
                accumEntry = newEntry;
            }
        }
    }

    /**
    * Apply the data binding information from a template program to the
    * target program.
    */
    void addTemplateDataToProgram( const osg::Program* templateProgram, osg::Program* program )
    {
        const osg::Program::FragDataBindingList& fbl = templateProgram->getFragDataBindingList();
        for( osg::Program::FragDataBindingList::const_iterator i = fbl.begin(); i != fbl.end(); ++i )
            program->addBindFragDataLocation( i->first, i->second );

        const osg::Program::UniformBlockBindingList& ubl = templateProgram->getUniformBlockBindingList();
        for( osg::Program::UniformBlockBindingList::const_iterator i = ubl.begin(); i != ubl.end(); ++i )
            program->addBindUniformBlock( i->first, i->second );
    }


    /**
    * Populates the specified Program with passed-in shaders.
    */
    void addShadersToProgram(const VirtualProgram::ShaderVector&      shaders, 
                             const VirtualProgram::AttribBindingList& attribBindings,
                             const VirtualProgram::AttribAliasMap&    attribAliases,
                             osg::Program*                            program )
    {
#ifdef USE_ATTRIB_ALIASES
        // apply any vertex attribute aliases. But first, sort them from longest to shortest 
        // so we don't get any overlap and bad replacements.
        VirtualProgram::AttribAliasVector sortedAliases;
        sortedAliases.reserve( attribAliases.size() );
        sortedAliases.insert(sortedAliases.begin(), attribAliases.begin(), attribAliases.end());
        std::sort( sortedAliases.begin(), sortedAliases.end(), s_attribAliasSortFunc );

        for( VirtualProgram::ShaderVector::const_iterator i = shaders.begin(); i != shaders.end(); ++i )
        {
            osg::Shader* shader = i->get();
            applyAttributeAliases( shader, sortedAliases );
        }
#endif

        // merge the shaders if necessary.
        if ( s_mergeShaders )
        {
            unsigned          vertVersion = 0;
            HeaderMap         vertHeaders;
            std::stringstream vertBody;

            unsigned          fragVersion = 0;
            HeaderMap         fragHeaders;
            std::stringstream fragBody;

            // parse the shaders, combining header lines and finding the highest version:
            for( VirtualProgram::ShaderVector::const_iterator i = shaders.begin(); i != shaders.end(); ++i )
            {
                osg::Shader* s = i->get();
                if ( s->getType() == osg::Shader::VERTEX )
                {
                    parseShaderForMerging( s->getShaderSource(), vertVersion, vertHeaders, vertBody );
                }
                else if ( s->getType() == osg::Shader::FRAGMENT )
                {
                    parseShaderForMerging( s->getShaderSource(), fragVersion, fragHeaders, fragBody );
                }
            }

            // write out the merged shader code:
            std::string vertBodyText;
            vertBodyText = vertBody.str();
            std::stringstream vertShaderBuf;
            if ( vertVersion > 0 )
                vertShaderBuf << "#version " << vertVersion << "\n";
            for( HeaderMap::const_iterator h = vertHeaders.begin(); h != vertHeaders.end(); ++h )
                vertShaderBuf << h->second << "\n";
            vertShaderBuf << vertBodyText << "\n";
            vertBodyText = vertShaderBuf.str();

            std::string fragBodyText;
            fragBodyText = fragBody.str();
            std::stringstream fragShaderBuf;
            if ( fragVersion > 0 )
                fragShaderBuf << "#version " << fragVersion << "\n";
            for( HeaderMap::const_iterator h = fragHeaders.begin(); h != fragHeaders.end(); ++h )
                fragShaderBuf << h->second << "\n";
            fragShaderBuf << fragBodyText << "\n";
            fragBodyText = fragShaderBuf.str();

            // add them to the program.
            program->addShader( new osg::Shader(osg::Shader::VERTEX, vertBodyText) );
            program->addShader( new osg::Shader(osg::Shader::FRAGMENT, fragBodyText) );

            if ( s_dumpShaders )
            {
                OE_NOTICE << LC 
                    << "\nMERGED VERTEX SHADER: \n\n" << vertBodyText << "\n\n"
                    << "MERGED FRAGMENT SHADER: \n\n" << fragBodyText << "\n" << std::endl;
            }
        }
        else
        {
            for( VirtualProgram::ShaderVector::const_iterator i = shaders.begin(); i != shaders.end(); ++i )
            {
                program->addShader( i->get() );
                if ( s_dumpShaders )
                {
                    OE_NOTIFY(osg::NOTICE,"")
                        << "----------\n"
                        << i->get()->getShaderSource() << std::endl;
                }
            }
        }

        // add the attribute bindings
        for( VirtualProgram::AttribBindingList::const_iterator abl = attribBindings.begin(); abl != attribBindings.end(); ++abl )
        {
            program->addBindAttribLocation( abl->first, abl->second );
        }
    }


    /**
    * Assemble a new OSG shader Program from the provided components.
    * Outputs the uniquely-identifying "key vector" and returns the new program.
    */
    osg::Program* buildProgram(const std::string&                  programName,
                               osg::State&                         state,
                               ShaderComp::FunctionLocationMap&    accumFunctions,
                               VirtualProgram::ShaderMap&          accumShaderMap,
                               VirtualProgram::AttribBindingList&  accumAttribBindings,
                               VirtualProgram::AttribAliasMap&     accumAttribAliases,
                               osg::Program*                       templateProgram,
                               VirtualProgram::ShaderVector&       outputKeyVector)
    {

#ifdef DEBUG_ACCUMULATION

        // test dump .. function map and shader map should always include identical data.
        OE_INFO << LC << "====PROGRAM: " << programName << "\n";

        // debug:
        OE_INFO << LC << "====FUNCTIONS:\n";

        for(ShaderComp::FunctionLocationMap::iterator i = accumFunctions.begin();
            i != accumFunctions.end();
            ++i)
        {
            ShaderComp::OrderedFunctionMap& ofm = i->second;
            for(ShaderComp::OrderedFunctionMap::iterator j = ofm.begin(); j != ofm.end(); ++j)
            {
                OE_INFO << LC 
                    << j->second._name << ", " << (j->second.accept(state)?"accepted":"rejected")
                    << std::endl;
            }
        }

        OE_INFO << LC << "====SHADERS:\n";
        for(VirtualProgram::ShaderMap::iterator i = accumShaderMap.begin();
            i != accumShaderMap.end();
            ++i)
        {
            OE_INFO << LC
                << i->first << ", "
                << (i->second.accept(state)?"accepted":"rejected") << ", "
                << i->second._overrideValue
                << std::endl;
        }
        OE_INFO << LC << "\n\n";
#endif

        // create new MAINs for this function stack.
        osg::Shader* vertMain = Registry::shaderFactory()->createVertexShaderMain( accumFunctions );
        osg::Shader* fragMain = Registry::shaderFactory()->createFragmentShaderMain( accumFunctions );

        // build a new "key vector" now that we've changed the shader map.
        // we call is a key vector because it uniquely identifies this shader program
        // based on its accumlated function set.
        for( VirtualProgram::ShaderMap::iterator i = accumShaderMap.begin(); i != accumShaderMap.end(); ++i )
        {
            outputKeyVector.push_back( i->data()._shader.get() );
        }

        // finally, add the mains (AFTER building the key vector .. we don't want or
        // need to mains in the key vector since they are completely derived from the
        // other elements of the key vector.)
        VirtualProgram::ShaderVector buildVector( outputKeyVector );
        buildVector.push_back( vertMain );
        buildVector.push_back( fragMain );

        if ( s_dumpShaders )
        {
            OE_NOTICE << LC << "\nPROGRAM: " << programName << " =============================\n" << std::endl;
        }

        // Create the new program.
        osg::Program* program = new osg::Program();
        program->setName( programName );
        addShadersToProgram( buildVector, accumAttribBindings, accumAttribAliases, program );
        addTemplateDataToProgram( templateProgram, program );

        return program;
    }
}

//------------------------------------------------------------------------

void
VirtualProgram::AttrStackMemory::remember(const osg::State&                 state,
                                          const VirtualProgram::AttrStack&  rhs,
                                          osg::Program*                     program)
{
    if ( state.getFrameStamp() )
    {
        unsigned contextID = state.getContextID();
        Item& item = _item[contextID];
        item.frameNumber = state.getFrameStamp()->getFrameNumber();
        item.attrStack = rhs;
        item.program = program;
    }
}

osg::Program*
VirtualProgram::AttrStackMemory::recall(const osg::State&                 state,
                                        const VirtualProgram::AttrStack&  rhs)
{
    osg::Program* program = 0L;

    const osg::FrameStamp* fs = state.getFrameStamp();
    if ( fs )
    {
        unsigned contextID = state.getContextID();
        Item& item = _item[contextID];

        // check frame number:
        if ( item.program.valid() && item.frameNumber == fs->getFrameNumber() )
        {
            // same, how compare the attribute stacks:
            if ( item.attrStack.size() == rhs.size() )
            {
                bool arraysMatch = true;
                for(int i=0; i<item.attrStack.size() && arraysMatch; ++i)
                {
                    if (item.attrStack[i] != rhs[i])
                    {
                        arraysMatch = false;
                    }
                }

                if (arraysMatch)
                {
                    program = item.program.get();
                }
            }
        }

        if ( !program )
        {
            item.frameNumber = fs->getFrameNumber();
            item.attrStack = rhs;
            item.program = 0L;
        }
    }

    return program;
}

//------------------------------------------------------------------------

VirtualProgram::ShaderEntry::ShaderEntry() :
_overrideValue(0)
{
    //nop
}

bool
VirtualProgram::ShaderEntry::accept(const osg::State& state) const
{
    return (!_accept.valid()) || (_accept->operator()(state) == true);
}

bool
VirtualProgram::ShaderEntry::operator < (const VirtualProgram::ShaderEntry& rhs) const
{
    if ( _shader->compare(*rhs._shader.get()) < 0 ) return true;
    if ( _overrideValue < rhs._overrideValue ) return true;
    if ( _accept.valid() && !rhs._accept.valid() ) return true;
    return false;
}

//------------------------------------------------------------------------

// same type as PROGRAM (for proper state sorting)
const osg::StateAttribute::Type VirtualProgram::SA_TYPE = osg::StateAttribute::PROGRAM;

VirtualProgram* 
VirtualProgram::getOrCreate(osg::StateSet* stateset)
{
    if ( !stateset )
        return 0L;

    VirtualProgram* vp = dynamic_cast<VirtualProgram*>( stateset->getAttribute(SA_TYPE) );
    if ( !vp )
    {
        vp = new VirtualProgram();
        vp->setInheritShaders(true);
        stateset->setAttributeAndModes( vp, osg::StateAttribute::ON );
    }
    return vp;
}

VirtualProgram* 
VirtualProgram::get(osg::StateSet* stateset)
{
    if ( !stateset )
        return 0L;

    return dynamic_cast<VirtualProgram*>( stateset->getAttribute(SA_TYPE) );
}

const VirtualProgram* 
VirtualProgram::get(const osg::StateSet* stateset)
{
    if ( !stateset )
        return 0L;

    return dynamic_cast<const VirtualProgram*>( stateset->getAttribute(SA_TYPE) );
}

VirtualProgram*
VirtualProgram::cloneOrCreate(const osg::StateSet* src, osg::StateSet* dest)
{
    if ( !dest )
        return 0L;

    const VirtualProgram* vp = 0L;

    if ( src )
    {
        vp = get( src );
    }

    if ( !vp )
    {
        return getOrCreate( dest );
    }

    else
    {
        VirtualProgram* cloneVP = osg::clone( vp, osg::CopyOp::DEEP_COPY_ALL );
        cloneVP->setInheritShaders(true);
        dest->setAttributeAndModes(cloneVP, osg::StateAttribute::ON);
        return cloneVP;
    }
}

VirtualProgram*
VirtualProgram::cloneOrCreate(osg::StateSet* stateset)
{
    return cloneOrCreate(stateset, stateset);
}

//------------------------------------------------------------------------


VirtualProgram::VirtualProgram( unsigned mask ) : 
_mask              ( mask ),
_active            ( true ),
_inherit           ( true ),
_inheritSet        ( false ),
_logShaders        ( false ),
_logPath           ( "" ),
_acceptCallbacksVaryPerFrame( false )
{
    // Note: we cannot set _active here. Wait until apply().
    // It will cause a conflict in the Registry.

    // check the the dump env var
    if ( ::getenv(OSGEARTH_DUMP_SHADERS) != 0L )
    {
        s_dumpShaders = true;
    }

    // check the merge env var
    if ( ::getenv(OSGEARTH_MERGE_SHADERS) != 0L )
    {
        s_mergeShaders = true;
    }

    // a template object to hold program data (so we don't have to dupliate all the 
    // osg::Program methods..)
    _template = new osg::Program();
}


VirtualProgram::VirtualProgram(const VirtualProgram& rhs, const osg::CopyOp& copyop ) :
osg::StateAttribute( rhs, copyop ),
_shaderMap         ( rhs._shaderMap ),
_mask              ( rhs._mask ),
_functions         ( rhs._functions ),
_inherit           ( rhs._inherit ),
_inheritSet        ( rhs._inheritSet ),
_logShaders        ( rhs._logShaders ),
_logPath           ( rhs._logPath ),
_template          ( osg::clone(rhs._template.get()) )
{    
    // Attribute bindings.
    const osg::Program::AttribBindingList &abl = rhs.getAttribBindingList();
    for( osg::Program::AttribBindingList::const_iterator attribute = abl.begin(); attribute != abl.end(); ++attribute )
    {
        addBindAttribLocation( attribute->first, attribute->second );
    }
}

VirtualProgram::~VirtualProgram()
{
    //NOP
}

int
VirtualProgram::compare(const osg::StateAttribute& sa) const
{
    // check the types are equal and then create the rhs variable
    // used by the COMPARE_StateAttribute_Parameter macros below.
    COMPARE_StateAttribute_Types(VirtualProgram,sa);

    // compare each parameter in turn against the rhs.
    COMPARE_StateAttribute_Parameter(_mask);
    COMPARE_StateAttribute_Parameter(_inherit);

    // compare the shader maps. Need to lock them while comparing.
    {
        //Threading::ScopedReadLock shared( _dataModelMutex );
        Threading::ScopedMutexLock lock( _dataModelMutex );
        
        if ( _shaderMap.size() < rhs._shaderMap.size() ) return -1;
        if ( _shaderMap.size() > rhs._shaderMap.size() ) return 1;

        ShaderMap::const_iterator lhsIter = _shaderMap.begin();
        ShaderMap::const_iterator rhsIter = rhs._shaderMap.begin();

        while( lhsIter != _shaderMap.end() )
        {
            //int keyCompare = lhsIter->key().compare( rhsIter->key() );
            //if ( keyCompare != 0 ) return keyCompare;

            if ( lhsIter->key() < rhsIter->key() ) return -1;
            if ( lhsIter->key() > rhsIter->key() ) return  1;

            const ShaderEntry& lhsEntry = lhsIter->data(); //lhsIter->second;
            const ShaderEntry& rhsEntry = rhsIter->data(); //rhsIter->second;

            if ( lhsEntry < rhsEntry ) return -1;
            if ( rhsEntry < lhsEntry ) return  1;

            lhsIter++;
            rhsIter++;
        }

        // compare the template settings.
        int templateCompare = _template->compare( *(rhs.getTemplate()) );
        if ( templateCompare != 0 ) return templateCompare;
    }

    return 0; // passed all the above comparison macros, must be equal.
}

void
VirtualProgram::addBindAttribLocation( const std::string& name, GLuint index )
{
    //Threading::ScopedWriteLock exclusive( _dataModelMutex );
    _dataModelMutex.lock();

#ifdef USE_ATTRIB_ALIASES
    _attribAliases[name] = Stringify() << "oe_attrib_" << index;
    _attribBindingList[_attribAliases[name]] = index;
#else
    _attribBindingList[name] = index;
#endif

    _dataModelMutex.unlock();
}

void
VirtualProgram::removeBindAttribLocation( const std::string& name )
{
    //Threading::ScopedWriteLock exclusive( _dataModelMutex );
    _dataModelMutex.lock();

#ifdef USE_ATTRIB_ALIASES
    std::map<std::string,std::string>::iterator i = _attribAliases.find(name);
    if ( i != _attribAliases.end() )
        _attribBindingList.erase(i->second);
#else
    _attribBindingList.erase(name);
#endif

    _dataModelMutex.unlock();
}

void
VirtualProgram::compileGLObjects(osg::State& state) const
{
    this->apply(state);
}

void
VirtualProgram::resizeGLObjectBuffers(unsigned maxSize)
{
    _programCacheMutex.lock();

    for (ProgramMap::iterator i = _programCache.begin(); i != _programCache.end(); ++i)
    {
        i->second._program->resizeGLObjectBuffers(maxSize);
    }

    _programCacheMutex.unlock();
}

void
VirtualProgram::releaseGLObjects(osg::State* state) const
{
    _programCacheMutex.lock();

    for (ProgramMap::const_iterator i = _programCache.begin(); i != _programCache.end(); ++i)
    {
        //if ( i->second->referenceCount() == 1 )
            i->second._program->releaseGLObjects(state);
    }

    _programCache.clear();

    _programCacheMutex.unlock();
}

osg::Shader*
VirtualProgram::getShader( const std::string& shaderID ) const
{
    Threading::ScopedMutexLock readonly( _dataModelMutex );
    const ShaderEntry* entry = _shaderMap.find( MAKE_SHADER_ID(shaderID) );
    return entry ? entry->_shader.get() : 0L;
}


osg::Shader*
VirtualProgram::setShader(const std::string&                 shaderID,
                          osg::Shader*                       shader,
                          osg::StateAttribute::OverrideValue ov)
{
    if ( !shader || shader->getType() ==  osg::Shader::UNDEFINED ) 
        return NULL;

    // set the inherit flag if it's not initialized
    if ( !_inheritSet )
    {
        setInheritShaders( true );
    }

    checkSharing();

    // set the name to the ID:
    shader->setName( shaderID );

    // pre-processes the shader's source to include GLES uniforms as necessary
    // (no-op on non-GLES)
    ShaderPreProcessor::run( shader );

    // lock the data model and insert the new shader.
    {
        _dataModelMutex.lock();
        //Threading::ScopedWriteLock exclusive( _dataModelMutex );

        ShaderEntry& entry = _shaderMap[MAKE_SHADER_ID(shaderID)];
        entry._shader        = shader;
        entry._overrideValue = ov;
        entry._accept        = 0L;

        _dataModelMutex.unlock();
    }

    return shader;
}


osg::Shader*
VirtualProgram::setShader(osg::Shader*                       shader,
                          osg::StateAttribute::OverrideValue ov)
{
    if ( !shader || shader->getType() == osg::Shader::UNDEFINED )
        return NULL;

    if ( shader->getName().empty() )
    {
        OE_WARN << LC << "setShader called but the shader name is not set" << std::endl;
        return 0L;
    }

    // set the inherit flag if it's not initialized
    if ( !_inheritSet )
    {
        setInheritShaders( true );
    }

    // pre-processes the shader's source to include GLES uniforms as necessary
    // (no-op on non-GLES)
    ShaderPreProcessor::run( shader );

    // lock the data model while changing it.
    {
        _dataModelMutex.lock();

        checkSharing();

        ShaderEntry& entry = _shaderMap[MAKE_SHADER_ID(shader->getName())];
        entry._shader        = shader;
        entry._overrideValue = ov;
        entry._accept        = 0L;

        _dataModelMutex.unlock();
    }

    return shader;
}


void
VirtualProgram::setFunction(const std::string&           functionName,
                            const std::string&           shaderSource,
                            ShaderComp::FunctionLocation location,
                            float                        ordering)
{
    setFunction(functionName, shaderSource, location, 0L, ordering);
}

void
VirtualProgram::setFunction(const std::string&           functionName,
                            const std::string&           shaderSource,
                            ShaderComp::FunctionLocation location,
                            ShaderComp::AcceptCallback*  accept,
                            float                        ordering)
{
    // set the inherit flag if it's not initialized
    if ( !_inheritSet )
    {
        setInheritShaders( true );
    }

    // lock the functions map while iterating and then modifying it:
    {
        _dataModelMutex.lock();

        checkSharing();

        OrderedFunctionMap& ofm = _functions[location];

        // if there's already a function by this name, remove it
        for( OrderedFunctionMap::iterator i = ofm.begin(); i != ofm.end(); )
        {
            ShaderComp::Function& f = i->second;
            if ( f._name.compare(functionName) == 0 )
            {
                OrderedFunctionMap::iterator j = i;
                ++j;
                ofm.erase( i );
                i = j;
            }
            else
            {
                ++i;
            }
        }
        
        ShaderComp::Function function;
        function._name   = functionName;
        function._accept = accept;
        ofm.insert( OrderedFunction(ordering, function) );

        // create and add the new shader function.
        osg::Shader::Type type = (int)location <= (int)LOCATION_VERTEX_CLIP ?
            osg::Shader::VERTEX : osg::Shader::FRAGMENT;

        osg::Shader* shader = new osg::Shader(type, shaderSource);
        shader->setName( functionName );

        // pre-processes the shader's source to include GLES uniforms as necessary
        ShaderPreProcessor::run( shader );

        ShaderEntry& entry = _shaderMap[MAKE_SHADER_ID(functionName)];
        entry._shader        = shader;
        entry._overrideValue = osg::StateAttribute::ON;
        entry._accept        = accept;

        _dataModelMutex.unlock();

    } // release lock
}

void 
VirtualProgram::setFunctionMinRange(const std::string& name, float minRange)
{
    // lock the functions map while making changes:
    _dataModelMutex.lock();

    checkSharing();

    ShaderComp::Function* function;
    if ( findFunction(name, _functions, &function) )
    {
        function->_minRange = minRange;
    }

    _dataModelMutex.unlock();
}

void 
VirtualProgram::setFunctionMaxRange(const std::string& name, float maxRange)
{
    // lock the functions map while making changes:
    _dataModelMutex.lock();

    checkSharing();

    ShaderComp::Function* function;
    if ( findFunction(name, _functions, &function) )
    {
        function->_maxRange = maxRange;
    }

    _dataModelMutex.unlock();
}

void
VirtualProgram::removeShader( const std::string& shaderID )
{
    // lock the functions map while making changes:
    Threading::ScopedMutexLock exclusive( _dataModelMutex );

    _shaderMap.erase( MAKE_SHADER_ID(shaderID) );

    for(FunctionLocationMap::iterator i = _functions.begin(); i != _functions.end(); ++i )
    {
        OrderedFunctionMap& ofm = i->second;
        for( OrderedFunctionMap::iterator j = ofm.begin(); j != ofm.end(); ++j )
        {
            if ( j->second._name.compare(shaderID) == 0 )
            {
                ofm.erase( j );

                // if the function map for this location is now empty,
                // remove the location map altogether.
                if ( ofm.size() == 0 )
                {
                    _functions.erase( i );
                }
                return;
            }
        }
    }
}


void
VirtualProgram::setInheritShaders( bool value )
{
    if ( _inherit != value || !_inheritSet )
    {
        _inherit = value;

        // clear the program cache please
        {
            _programCacheMutex.lock();
            _programCache.clear();
            _programCacheMutex.unlock();
        }

        _inheritSet = true;
    }
}


void
VirtualProgram::apply( osg::State& state ) const
{
    if (_active.isSetTo(false))
    {
        return;
    }
    else if ( !_active.isSet() )
    {
        _active = Registry::capabilities().supportsGLSL();
    }
    
    const unsigned contextID = state.getContextID();

    if (_shaderMap.empty() && !_inheritSet)
    {
        // If there's no data in the VP, and never has been, unload any existing program.
        // NOTE: OSG's State processor creates a "global default attribute" for each type.
        // Sine we have no way of knowing whether the user created the VP or OSG created it
        // as the default fallback, we use the "_inheritSet" flag to differeniate. This
        // prevents any shader leakage from a VP-enabled node.
        if ( state.getLastAppliedProgramObject() != 0L )
        {
            const osg::GL2Extensions* extensions = osg::GL2Extensions::Get(contextID,true);
            extensions->glUseProgram( 0 );
            state.setLastAppliedProgramObject(0);
        }
        return;
    }

    osg::ref_ptr<osg::Program> program;
    
    // Negate osg::State's last-attribute-applied tracking for 
    // VirtualProgram, since it cannot detect a VP that is reached from
    // different node/attribute paths. We replace this with the 
    // stack-memory construct below which will "remember" whether 
    // the VP has already been applied during the current frame using
    // the same an identical attribute stack.
    state.haveAppliedAttribute(this->SA_TYPE);

#ifdef USE_STACK_MEMORY
    bool programRecalled = false;
    const AttrStack* stack = StateEx::getProgramStack(state);
    if ( stack)
    {
        program = _vpStackMemory.recall(state, *stack);
        programRecalled = program.valid();
    }
#endif // USE_STACK_MEMORY
    
    // We need to tracks whether there are any accept callbacks, because if so
    // we cannot store the program in stack memory -- the accept callback can
    // exclude shaders based on any condition.
    bool acceptCallbacksVary = _acceptCallbacksVaryPerFrame;

    if ( !program.valid() )
    {
        // Access the resuable shader map for this context. Bypasses reallocation overhead.
        ApplyVars& local = _apply[contextID];

        local.accumShaderMap.clear();
        local.accumAttribBindings.clear();
        local.accumAttribAliases.clear();
        local.keyVector.clear();
    
        // If we are inheriting, build the active shader map up to this point
        // (but not including this VP).
        if ( _inherit )
        {
            accumulateShaders(
                state,
                _mask,
                local.accumShaderMap,
                local.accumAttribBindings,
                local.accumAttribAliases,
                acceptCallbacksVary);
        }
        
        // Next, add the shaders from this VP.
        {
            //Threading::ScopedReadLock readonly(_dataModelMutex);
            _dataModelMutex.lock();

            for( ShaderMap::const_iterator i = _shaderMap.begin(); i != _shaderMap.end(); ++i )
            {
                if ( i->data().accept(state) )
                {
                    addToAccumulatedMap( local.accumShaderMap, i->key(), i->data() );
                }
            }

            const AttribBindingList& abl = this->getAttribBindingList();
            local.accumAttribBindings.insert( abl.begin(), abl.end() );

    #ifdef USE_ATTRIB_ALIASES
            const AttribAliasMap& aliases = this->getAttribAliases();
            local.accumAttribAliases.insert( aliases.begin(), aliases.end() );
    #endif
            
            _dataModelMutex.unlock();
        }

        // next, assemble a list of the shaders in the map so we can use it as our
        // program cache key.
        // (Note: at present, the "cache key" does not include any information on the vertex
        // attribute bindings. Technically it should, but in practice this might not be an
        // issue; it is unlikely one would have two identical shader programs with different
        // bindings.)
        for( ShaderMap::iterator i = local.accumShaderMap.begin(); i != local.accumShaderMap.end(); ++i )
        {
            local.keyVector.push_back( i->data()._shader.get() );
        }

        // current frame number, for shader program expiry.
        unsigned frameNumber = state.getFrameStamp() ? state.getFrameStamp()->getFrameNumber() : 0;

        // look up the program:
        {
            _programCacheMutex.lock();
            const_cast<VirtualProgram*>(this)->readProgramCache(local.keyVector, frameNumber, program);
            _programCacheMutex.unlock();
        }

        // if not found, lock and build it:
        if ( !program.valid() )
        {
            // build a new set of accumulated functions, to support the creation of main()
            ShaderComp::FunctionLocationMap accumFunctions;
            accumulateFunctions( state, accumFunctions );

            // now double-check the program cache, and failing that, build the
            // new shader Program.
            {
                Threading::ScopedMutexLock lock(_programCacheMutex);

                // double-check: look again to negate race conditions
                const_cast<VirtualProgram*>(this)->readProgramCache(local.keyVector, frameNumber, program);
                if ( !program.valid() )
                {
                    local.keyVector.clear();

                    //OE_NOTICE << LC << "Building new Program for VP " << getName() << std::endl;

                    program = buildProgram(
                        getName(),
                        state,
                        accumFunctions,
                        local.accumShaderMap, 
                        local.accumAttribBindings, 
                        local.accumAttribAliases, 
                        _template.get(),
                        local.keyVector);

                    if ( _logShaders && program.valid() )
                    {
                        std::stringstream buf;
                        for (unsigned i=0; i < program->getNumShaders(); i++)
                        {
                            buf << program->getShader(i)->getShaderSource() << std::endl << std::endl;
                        }

                        if ( _logPath.length() > 0 )
                        {
                            std::fstream outStream;
                            outStream.open(_logPath.c_str(), std::ios::out);
                            if (outStream.fail())
                            {
                                OE_WARN << LC << "Unable to open " << _logPath << " for logging shaders." << std::endl;
                            }
                            else
                            {
                                outStream << buf.str();
                                outStream.close();
                            }
                        }
                        else
                        {
                          OE_NOTICE << LC << "Shader source: " << getName() << std::endl << "===============" << std::endl << buf.str() << std::endl << "===============" << std::endl;
                        }
                    }

                    // global sharing.
                    Registry::programSharedRepo()->share( program );

                    // finally, put own new program in the cache.
                    ProgramEntry& pe = _programCache[local.keyVector];
                    pe._program = program.get();
                    pe._frameLastUsed = frameNumber;

                    // purge expired programs.
                    const_cast<VirtualProgram*>(this)->removeExpiredProgramsFromCache(state, frameNumber);
                }
            }
        }
    }

    // finally, apply the program attribute.
    if ( program.valid() )
    {
#ifdef USE_STACK_MEMORY
        // remember this program selection in case this VP is applied again
        // during the same frame.
        if (programRecalled == false        &&   // recalled a program? not necessary
            getDataVariance() != DYNAMIC    &&   // DYNAMIC variance? might change during ST cull; no memory
            !acceptCallbacksVary            &&   // accept callbacks vary per frame? cannot use memory
            stack != 0L )
        {
            _vpStackMemory.remember(state, *stack, program.get());
        }
#endif // USE_STACK_MEMORY

        osg::Program::PerContextProgram* pcp;

#if OSG_VERSION_GREATER_OR_EQUAL(3,3,4)
        pcp = program->getPCP( state );
#else
        pcp = program->getPCP( contextID );
#endif
        bool useProgram = state.getLastAppliedProgramObject() != pcp;

#ifdef DEBUG_APPLY_COUNTS
        if ( state.getFrameStamp() && state.getFrameStamp()->getFrameNumber()%60==0)
        {
            // debugging            
            static int s_framenum = 0;
            static Threading::Mutex s_mutex;
            static std::map< const VirtualProgram*, std::pair<int,int> > s_counts;

            Threading::ScopedMutexLock lock(s_mutex);

            int framenum = state.getFrameStamp()->getFrameNumber();
            if ( framenum > s_framenum )
            {
                OE_NOTICE << LC << "Use program in last frame: " << std::endl;
                for(std::map<const VirtualProgram*,std::pair<int,int> >::iterator i = s_counts.begin(); i != s_counts.end(); ++i)
                {
                    std::pair<int,int>& counts = i->second;
                    OE_NOTICE << LC << "  " 
                        << i->first->getName() << " : attemped=" << counts.first << ", applied=" << counts.second << "\n";
                }
                s_framenum = framenum;
                s_counts.clear();
            }
            s_counts[this].first++;
            if ( useProgram )
                s_counts[this].second++;
        }
#endif

        if ( useProgram )
        {
            if( pcp->needsLink() )
                program->compileGLObjects( state );

            if( pcp->isLinked() )
            {
                if( osg::isNotifyEnabled(osg::INFO) )
                    pcp->validateProgram();

                pcp->useProgram();
                state.setLastAppliedProgramObject( pcp );
            }
            else
            {
                // program not usable, fallback to fixed function.
                const osg::GL2Extensions* extensions = osg::GL2Extensions::Get(contextID, true);
                extensions->glUseProgram( 0 );
                state.setLastAppliedProgramObject(0);
                OE_DEBUG << LC << "Program link failure!" << std::endl;
            }
        }

        //program->apply( state );

#if 0 // test code for detecting race conditions
        for(int i=0; i<10000; ++i) {
            state.setLastAppliedProgramObject(0L);
            program->apply( state );
        }
#endif
    }
}

void
VirtualProgram::removeExpiredProgramsFromCache(osg::State& state, unsigned frameNumber)
{
    if ( frameNumber > 0 && _programCache.size() > MAX_PROGRAM_CACHE_SIZE )
    {
        // ASSUME a mutex lock on the cache.
        for(ProgramMap::iterator k=_programCache.begin(); k!=_programCache.end(); )
        {
            if ( frameNumber - k->second._frameLastUsed > 2 )
            {
                if ( k->second._program->referenceCount() == 1 )
                {
                    k->second._program->releaseGLObjects(&state);
                }
                k = _programCache.erase(k);
            }
            else
            {
                ++k;
            }
        }
    }
}

bool
VirtualProgram::readProgramCache(const ShaderVector& vec, unsigned frameNumber, osg::ref_ptr<osg::Program>& program)
{
    ProgramMap::iterator p = _programCache.find( vec );
    if ( p != _programCache.end() )
    {
        // update as current..
        p->second._frameLastUsed = frameNumber;
        program = p->second._program.get();
    }
    return program.valid();
}


bool
VirtualProgram::checkSharing()
{
  if ( ::getenv("OSGEARTH_SHARED_VP_WARNING") && getNumParents() > 1)
  {
      OE_WARN << LC << "Modified VirtualProgram may be shared." << std::endl;
      return true;
  }

  return false;
}

void
VirtualProgram::getFunctions( FunctionLocationMap& out ) const
{
    // make a safe copy of the functions map.
    _dataModelMutex.lock();
    out = _functions;
    _dataModelMutex.unlock();
}

void
VirtualProgram::getShaderMap( ShaderMap& out ) const
{
    // make a safe copy of the functions map.
    _dataModelMutex.lock();
    out = _shaderMap;
    _dataModelMutex.unlock();
}

void
VirtualProgram::accumulateFunctions(const osg::State&                state,
                                    ShaderComp::FunctionLocationMap& result) const
{
    // This method searches the state's attribute stack and accumulates all 
    // the user functions (including those in this program).
    if ( _inherit )
    {
        const AttrStack* av = StateEx::getProgramStack(state);
        if ( av && av->size() > 0 )
        {
            // find the closest VP that doesn't inherit:
            unsigned start;
            for( start = (int)av->size()-1; start > 0; --start )
            {
                const VirtualProgram* vp = dynamic_cast<const VirtualProgram*>( (*av)[start].first );
                if ( vp && (vp->_mask & _mask) && vp->_inherit == false )
                    break;
            }

            // collect functions from there on down.
            for( unsigned i=start; i<av->size(); ++i )
            {
                const VirtualProgram* vp = dynamic_cast<const VirtualProgram*>( (*av)[i].first );
                if ( vp && (vp->_mask && _mask) && (vp != this) )
                {
                    FunctionLocationMap rhs;
                    vp->getFunctions( rhs );

                    for( FunctionLocationMap::const_iterator j = rhs.begin(); j != rhs.end(); ++j )
                    {
                        const OrderedFunctionMap& source = j->second;
                        OrderedFunctionMap&       dest   = result[j->first];

                        for( OrderedFunctionMap::const_iterator k = source.begin(); k != source.end(); ++k )
                        {
                            if ( k->second.accept(state) )
                            {
                                // remove/override an existing function with the same name
                                for( OrderedFunctionMap::iterator exists = dest.begin(); exists != dest.end(); ++exists )
                                {
                                    if ( exists->second._name.compare( k->second._name ) == 0 )
                                    {
                                        dest.erase(exists);
                                        break;
                                    }
                                }
                                dest.insert( *k );
                            }
                        }
                    }
                }
            }
        }
    }

    // add the local ones too:
    {
        //Threading::ScopedReadLock readonly( _dataModelMutex );
        _dataModelMutex.lock();

        for( FunctionLocationMap::const_iterator j = _functions.begin(); j != _functions.end(); ++j )
        {
            const OrderedFunctionMap& source = j->second;
            OrderedFunctionMap&       dest   = result[j->first];

            for( OrderedFunctionMap::const_iterator k = source.begin(); k != source.end(); ++k )
            {
                if ( k->second.accept(state) )
                {
                    // remove/override an existing function with the same name
                    for( OrderedFunctionMap::iterator exists = dest.begin(); exists != dest.end(); ++exists )
                    {
                        if ( exists->second._name.compare( k->second._name ) == 0 )
                        {
                            dest.erase(exists);
                            break;
                        }
                    }
                    dest.insert( *k );
                }
            }
        }

        _dataModelMutex.unlock();
    }
}



void
VirtualProgram::accumulateShaders(const osg::State&  state, 
                                  unsigned           mask,
                                  ShaderMap&         accumShaderMap,
                                  AttribBindingList& accumAttribBindings,
                                  AttribAliasMap&    accumAttribAliases,
                                  bool&              acceptCallbacksVary)
{
    acceptCallbacksVary = false;

    const AttrStack* av = StateEx::getProgramStack(state);
    if ( av && av->size() > 0 )
    {
        // find the deepest VP that doesn't inherit:
        unsigned start = 0;
        for( start = (int)av->size()-1; start > 0; --start )
        {
            const VirtualProgram* vp = dynamic_cast<const VirtualProgram*>( (*av)[start].first );
            if ( vp && (vp->_mask & mask) && vp->_inherit == false )
                break;
        }

        // collect shaders from there to here:
        for( unsigned i=start; i<av->size(); ++i )
        {
            const VirtualProgram* vp = dynamic_cast<const VirtualProgram*>( (*av)[i].first );
            if ( vp && (vp->_mask && mask) )
            {
                if (vp->getAcceptCallbacksVaryPerFrame())
                {
                    acceptCallbacksVary = true;
                }

                // thread-safely adds the other vp's shaders to our accumulation map
                vp->addShadersToAccumulationMap( accumShaderMap, state );

                const AttribBindingList& abl = vp->getAttribBindingList();
                accumAttribBindings.insert( abl.begin(), abl.end() );

#ifdef USE_ATTRIB_ALIASES
                const AttribAliasMap& aliases = vp->getAttribAliases();
                accumAttribAliases.insert( aliases.begin(), aliases.end() );
#endif
            }
        }
    }
}

void
VirtualProgram::addShadersToAccumulationMap(VirtualProgram::ShaderMap& accumMap,
                                            const osg::State&          state) const
{
    //Threading::ScopedReadLock shared( _dataModelMutex );
    _dataModelMutex.lock();

    for( ShaderMap::const_iterator i = _shaderMap.begin(); i != _shaderMap.end(); ++i )
    {
        if ( i->data().accept(state) )
        {
            addToAccumulatedMap(accumMap, i->key(), i->data());
        }
    }

    _dataModelMutex.unlock();
}


void
VirtualProgram::getShaders(const osg::State&                        state,
                           std::vector<osg::ref_ptr<osg::Shader> >& output)
{
    ShaderMap         shaders;
    AttribBindingList bindings;
    AttribAliasMap    aliases;
    bool              acceptCallbacksVary;

    // build the collection:
    accumulateShaders(state, ~0, shaders, bindings, aliases, acceptCallbacksVary);

    // pre-allocate space:
    output.reserve( shaders.size() );

    // copy to output.
    for(ShaderMap::iterator i = shaders.begin(); i != shaders.end(); ++i)
    {
        output.push_back( i->data()._shader.get() );
    }
}

void VirtualProgram::setShaderLogging( bool log )
{
    setShaderLogging(log, "");
}

void VirtualProgram::setShaderLogging( bool log, const std::string& filepath )
{
    _logShaders = log;
    _logPath = filepath;
}

bool VirtualProgram::getAcceptCallbacksVaryPerFrame() const
{
    return _acceptCallbacksVaryPerFrame;
}

void VirtualProgram::setAcceptCallbacksVaryPerFrame(bool acceptCallbacksVaryPerFrame)
{
    _acceptCallbacksVaryPerFrame = acceptCallbacksVaryPerFrame;
}
