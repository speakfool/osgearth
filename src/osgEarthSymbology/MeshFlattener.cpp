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

#include <osgEarthSymbology/MeshConsolidator>
#include <osgEarthSymbology/MeshFlattener>
#include <osgEarth/StateSetCache>
#include <osgUtil/Optimizer>
#include <osgDB/WriteFile>
#include <osg/Billboard>

using namespace osgEarth;
using namespace osgEarth::Symbology;

#define LC "[MeshFlattener] "

using namespace osgEarth;


/********************************/
PrepareForOptimizationVisitor::PrepareForOptimizationVisitor():
osg::NodeVisitor( osg::NodeVisitor::TRAVERSE_ALL_CHILDREN )
{
    setNodeMaskOverride(~0);
}

void PrepareForOptimizationVisitor::apply(osg::Node& node)
{
    node.setUserData(0);
    node.setUserDataContainer(0);
    node.setName("");
    node.setDataVariance(osg::Object::STATIC);
    node.setCullCallback(0);
    node.setEventCallback(0);
    node.setUpdateCallback(0);
    traverse(node);
}

/********************************/
    FlattenSceneGraphVisitor::FlattenSceneGraphVisitor():
osg::NodeVisitor( osg::NodeVisitor::TRAVERSE_ALL_CHILDREN )
{
    setNodeMaskOverride(~0);
}

    void FlattenSceneGraphVisitor::apply(osg::Node& node)
    {
        osg::ref_ptr< osg::StateSet > ss = node.getStateSet();
        if (ss)
        {
            pushStateSet(ss.get());
        }
        traverse(node);
        if (ss)
        {
            popStateSet();
        }
    }

     void FlattenSceneGraphVisitor::apply(osg::Geode& geode)
    {
        osg::Billboard* billboard = dynamic_cast< osg::Billboard* >(&geode);
        // Special case, skip billboards since we can't cluster them.
        if (billboard)
        {
            return;
        }

        osg::ref_ptr< osg::StateSet > ss = geode.getStateSet();
        if (ss)
        {
            pushStateSet(ss.get());
        }

        for (unsigned int i = 0; i < geode.getNumDrawables(); i++)
        {
            osg::Geometry* geometry = geode.getDrawable(i)->asGeometry();
            if (geometry)
            {
                osg::ref_ptr< osg::StateSet > geomSS = geometry->getStateSet();
                if (geomSS.get())
                {
                    pushStateSet( geomSS.get() );
                }

                GeometryVector& geometries = _geometries[_ssStack];
                geometries.push_back(geometry);

                if (geomSS.get())
                {
                    popStateSet();
                }

                
            }
        }
        
        if (ss)
        {
            popStateSet();
        }

    }

    void FlattenSceneGraphVisitor::pushStateSet(osg::StateSet* stateSet)
    {
        _ssStack.push_back(stateSet);
    }

    void FlattenSceneGraphVisitor::popStateSet()
    {
        _ssStack.pop_back();
    }

    osg::Node* FlattenSceneGraphVisitor::build()
    {
        // Build a group that contains one geode per group
        osg::Group* result = new osg::Group;

        OE_DEBUG << "We have " << _geometries.size() << " stateset stacks" << std::endl;

        unsigned int i = 0;
        for (StateSetStackToGeometryMap::iterator itr = _geometries.begin(); itr != _geometries.end(); ++itr)
        {
            OE_DEBUG << LC << "StateSetStack " << i++ << " has " << itr->second.size() << " geometries " << std::endl;
            
            // Merge all of the statesets
            osg::StateSet* ss = new osg::StateSet();
            for (StateSetStack::const_iterator ssItr = itr->first.begin(); ssItr != itr->first.end(); ++ssItr)
            {
                ss->merge(*(ssItr->get()));
            }

            osg::Geode* geode = new osg::Geode;
            geode->setStateSet(ss);
            // Add all of the drawables to the new Geode
            for (GeometryVector::iterator gItr = itr->second.begin(); gItr != itr->second.end(); ++gItr)
            {
                osg::Geometry* g = gItr->get();
                // Remove any stateset that might be on the Geometry
                g->setStateSet(0);
                geode->addDrawable( g );
            }
            result->addChild(geode);
            
            // Consolidate all the drawables in the geode.
            MeshConsolidator::run(*geode);
        }

        // Run MERGE_GEOMETRY so that it will merge all the primitive sets
        osgUtil::Optimizer opt;
        opt.optimize( result, 
            osgUtil::Optimizer::MERGE_GEOMETRY
            );
       
        //osgDB::writeNodeFile(*result, "clustered.osg");

        return result;
    }


/********************************/
void MeshFlattener::run( osg::Group* group )
{
    // Do all that we can so the optimizer will actually do it's job.
    PrepareForOptimizationVisitor v;
    group->accept(v);

    // Remove any transforms 
    osgUtil::Optimizer optimizer;
    optimizer.optimize(group, osgUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS_DUPLICATING_SHARED_SUBGRAPHS);

    // Share all statesets and attributes so we can just do a pointer based stack.
    osg::ref_ptr< StateSetCache > sscache = new StateSetCache();
    sscache->optimize( group );

    // Now, collect all the geodes and merge them
    FlattenSceneGraphVisitor flatten;
    group->accept(flatten);

    // Remove all the old children.
    group->removeChildren(0, group->getNumChildren());

    // Add the new flat graph.
    group->addChild(flatten.build());
}
