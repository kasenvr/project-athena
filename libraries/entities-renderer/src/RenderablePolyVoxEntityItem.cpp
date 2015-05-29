//
//  RenderablePolyVoxEntityItem.cpp
//  libraries/entities-renderer/src/
//
//  Created by Seth Alves on 5/19/15.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <QByteArray>


#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/string_cast.hpp>

#include <gpu/GPUConfig.h>

#include <DeferredLightingEffect.h>
#include <Model.h>
#include <PerfStat.h>

#include <PolyVoxCore/CubicSurfaceExtractorWithNormals.h>
#include <PolyVoxCore/MarchingCubesSurfaceExtractor.h>
#include <PolyVoxCore/SurfaceMesh.h>
#include <PolyVoxCore/SimpleVolume.h>
#include <PolyVoxCore/Raycast.h>
#include <PolyVoxCore/Material.h>

#include "model/Geometry.h"
#include "gpu/GLBackend.h"
#include "EntityTreeRenderer.h"
#include "RenderablePolyVoxEntityItem.h"


EntityItemPointer RenderablePolyVoxEntityItem::factory(const EntityItemID& entityID, const EntityItemProperties& properties) {
    return EntityItemPointer(new RenderablePolyVoxEntityItem(entityID, properties));
}

RenderablePolyVoxEntityItem::~RenderablePolyVoxEntityItem() {
    delete _volData;
}

void RenderablePolyVoxEntityItem::setVoxelVolumeSize(glm::vec3 voxelVolumeSize) {

    if (_volData && voxelVolumeSize == _voxelVolumeSize) {
        return;
    }

    qDebug() << "resetting voxel-space size";

    PolyVoxEntityItem::setVoxelVolumeSize(voxelVolumeSize);

    if (_volData) {
        delete _volData;
    }

    PolyVox::Vector3DInt32 lowCorner(0, 0, 0);
    PolyVox::Vector3DInt32 highCorner(_voxelVolumeSize[0] - 1, // -1 because these corners are inclusive
                                      _voxelVolumeSize[1] - 1,
                                      _voxelVolumeSize[2] - 1);

    _volData = new PolyVox::SimpleVolume<uint8_t>(PolyVox::Region(lowCorner, highCorner));
}


void RenderablePolyVoxEntityItem::setVoxelData(QByteArray voxelData) {
    if (voxelData == _voxelData) {
        return;
    }
    PolyVoxEntityItem::setVoxelData(voxelData);
    decompressVolumeData();
}


glm::mat4 RenderablePolyVoxEntityItem::voxelToWorldMatrix() const {
    glm::vec3 scale = getDimensions() / _voxelVolumeSize; // meters / voxel-units
    glm::mat4 scaled = glm::scale(glm::mat4(), scale);
    glm::mat4 centerToCorner = glm::translate(scaled, _voxelVolumeSize / -2.0f);
    glm::mat4 rotation = glm::mat4_cast(getRotation());
    glm::mat4 translation = glm::translate(getCenterPosition());
    return translation * rotation * centerToCorner;
}

glm::mat4 RenderablePolyVoxEntityItem::worldToVoxelMatrix() const {
    glm::mat4 worldToModelMatrix = glm::inverse(voxelToWorldMatrix());
    return worldToModelMatrix;

}

void RenderablePolyVoxEntityItem::setSphereInVolume(glm::vec3 center, float radius, uint8_t toValue) {
    // This three-level for loop iterates over every voxel in the volume
    for (int z = 0; z < _volData->getDepth(); z++) {
        for (int y = 0; y < _volData->getHeight(); y++) {
            for (int x = 0; x < _volData->getWidth(); x++) {
                // Store our current position as a vector...
                glm::vec3 pos(x, y, z);
                // And compute how far the current position is from the center of the volume
                float fDistToCenter = glm::distance(pos, center);
                // If the current voxel is less than 'radius' units from the center then we make it solid.
                if (fDistToCenter <= radius) {
                    _volData->setVoxelAt(x, y, z, toValue);
                }
            }
        }
    }
    compressVolumeData();
    _needsModelReload = true;
}

void RenderablePolyVoxEntityItem::setSphere(glm::vec3 centerWorldCoords, float radiusWorldCoords, uint8_t toValue) {
    // glm::vec3 centerVoxelCoords = worldToVoxelCoordinates(centerWorldCoords);
    glm::vec4 centerVoxelCoords = worldToVoxelMatrix() * glm::vec4(centerWorldCoords, 1.0f);
    glm::vec3 scale = getDimensions() / _voxelVolumeSize; // meters / voxel-units
    float scaleY = scale[0];
    float radiusVoxelCoords = radiusWorldCoords / scaleY;
    setSphereInVolume(glm::vec3(centerVoxelCoords), radiusVoxelCoords, toValue);
}

void RenderablePolyVoxEntityItem::getModel() {
    if (!_volData) {
        // this will cause the allocation of _volData
        setVoxelVolumeSize(_voxelVolumeSize);
    }

    // A mesh object to hold the result of surface extraction
    PolyVox::SurfaceMesh<PolyVox::PositionMaterialNormal> polyVoxMesh;

    switch (_voxelSurfaceStyle) {
        case PolyVoxEntityItem::SURFACE_MARCHING_CUBES: {
            PolyVox::MarchingCubesSurfaceExtractor<PolyVox::SimpleVolume<uint8_t>> surfaceExtractor
                (_volData, _volData->getEnclosingRegion(), &polyVoxMesh);
            surfaceExtractor.execute();
            break;
        }
        case PolyVoxEntityItem::SURFACE_CUBIC: {
            PolyVox::CubicSurfaceExtractorWithNormals<PolyVox::SimpleVolume<uint8_t>> surfaceExtractor
                (_volData, _volData->getEnclosingRegion(), &polyVoxMesh);
            surfaceExtractor.execute();
            break;
        }
    }

    // convert PolyVox mesh to a Sam mesh
    model::Mesh* mesh = new model::Mesh();
    model::MeshPointer meshPtr(mesh);

    const std::vector<uint32_t>& vecIndices = polyVoxMesh.getIndices();
    auto indexBuffer = new gpu::Buffer(vecIndices.size() * sizeof(uint32_t), (gpu::Byte*)vecIndices.data());
    auto indexBufferPtr = gpu::BufferPointer(indexBuffer);
    mesh->setIndexBuffer(gpu::BufferView(indexBufferPtr, gpu::Element(gpu::SCALAR, gpu::UINT32, gpu::RAW)));


    const std::vector<PolyVox::PositionMaterialNormal>& vecVertices = polyVoxMesh.getVertices();
    auto vertexBuffer = new gpu::Buffer(vecVertices.size() * sizeof(PolyVox::PositionMaterialNormal),
                                        (gpu::Byte*)vecVertices.data());
    auto vertexBufferPtr = gpu::BufferPointer(vertexBuffer);
    mesh->setVertexBuffer(gpu::BufferView(vertexBufferPtr,
                                          0,
                                          vertexBufferPtr->getSize() - sizeof(float) * 3,
                                          sizeof(PolyVox::PositionMaterialNormal),
                                          gpu::Element(gpu::VEC3, gpu::FLOAT, gpu::RAW)));
    mesh->addAttribute(gpu::Stream::NORMAL,
                       gpu::BufferView(vertexBufferPtr,
                                       sizeof(float) * 3,
                                       vertexBufferPtr->getSize() - sizeof(float) * 3,
                                       sizeof(PolyVox::PositionMaterialNormal),
                                       gpu::Element(gpu::VEC3, gpu::FLOAT, gpu::RAW)));

    // auto normalAttrib = mesh->getAttributeBuffer(gpu::Stream::NORMAL);
    // for (auto normal = normalAttrib.begin<glm::vec3>(); normal != normalAttrib.end<glm::vec3>(); normal++) {
    //     (*normal) = -(*normal);
    // }

    qDebug() << "---- vecIndices.size() =" << vecIndices.size();
    qDebug() << "---- vecVertices.size() =" << vecVertices.size();

    _modelGeometry.setMesh(meshPtr);
    _needsModelReload = false;
}

void RenderablePolyVoxEntityItem::render(RenderArgs* args) {
    PerformanceTimer perfTimer("RenderablePolyVoxEntityItem::render");
    assert(getType() == EntityTypes::PolyVox);

    if (_needsModelReload) {
        getModel();
    }
    
    Transform transformToCenter = getTransformToCenter();
    transformToCenter.setScale(getDimensions() / _voxelVolumeSize);

    auto mesh = _modelGeometry.getMesh();
    Q_ASSERT(args->_batch);
    gpu::Batch& batch = *args->_batch;
    batch.setModelTransform(transformToCenter);
    batch.setInputFormat(mesh->getVertexFormat());
    batch.setInputBuffer(gpu::Stream::POSITION, mesh->getVertexBuffer());
    batch.setInputBuffer(gpu::Stream::NORMAL,
                         mesh->getVertexBuffer()._buffer,
                         sizeof(float) * 3,
                         mesh->getVertexBuffer()._stride);
    batch.setIndexBuffer(gpu::UINT32, mesh->getIndexBuffer()._buffer, 0);
    batch.drawIndexed(gpu::TRIANGLES, mesh->getNumIndices(), 0);
    
    RenderableDebugableEntityItem::render(this, args);
}

class RaycastFunctor
{
public:
    RaycastFunctor() : _result(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)) { }
    bool operator()(PolyVox::SimpleVolume<unsigned char>::Sampler& sampler)
    {
        if (sampler.getVoxel() == 0) {
            return true; // keep raycasting
        }
        PolyVox::Vector3DInt32 positionIndex = sampler.getPosition();
        _result = glm::vec4(positionIndex.getX(), positionIndex.getY(), positionIndex.getZ(), 1.0f);
        return false;
    }
    glm::vec4 _result;
};

bool RenderablePolyVoxEntityItem::findDetailedRayIntersection(const glm::vec3& origin,
                                                              const glm::vec3& direction,
                                                              bool& keepSearching,
                                                              OctreeElement*& element,
                                                              float& distance, BoxFace& face, 
                                                              void** intersectedObject,
                                                              bool precisionPicking) const
{
    if (_needsModelReload || !precisionPicking) {
        // just intersect with bounding box
        return true;
    }

    glm::mat4 wtvMatrix = worldToVoxelMatrix();
    glm::vec3 farPoint = origin + direction;
    glm::vec4 originInVoxel = wtvMatrix * glm::vec4(origin, 1.0f);
    glm::vec4 farInVoxel = wtvMatrix * glm::vec4(farPoint, 1.0f);
    glm::vec4 directionInVoxel = farInVoxel - originInVoxel;

    PolyVox::Vector3DFloat start(originInVoxel[0], originInVoxel[1], originInVoxel[2]);
    PolyVox::Vector3DFloat pvDirection(directionInVoxel[0], directionInVoxel[1], directionInVoxel[2]);
    pvDirection.normalise();

    // the PolyVox ray intersection code requires a near and far point.
    glm::vec3 scale = getDimensions() / _voxelVolumeSize; // meters / voxel-units
    float distanceToEntity = glm::distance(origin, getPosition());
    float largestDimension = glm::max(getDimensions()[0], getDimensions()[1], getDimensions()[2]);
    // set ray cast length to long enough to cover all of the voxel space
    pvDirection *= (distanceToEntity + largestDimension) / glm::min(scale[0], scale[1], scale[2]);

    PolyVox::RaycastResult raycastResult;
    RaycastFunctor callback;
    raycastResult = PolyVox::raycastWithDirection(_volData, start, pvDirection, callback);

    if (raycastResult == PolyVox::RaycastResults::Completed) {
        // the ray completed its path -- nothing was hit.
        return false;
    }

    glm::vec4 intersectedWorldPosition = voxelToWorldMatrix() * callback._result;

    distance = glm::distance(glm::vec3(intersectedWorldPosition), origin);

    face = BoxFace::MIN_X_FACE; // XXX

    return true;
}


// compress the data in _volData and save the results.  The compressed form is used during
// saves to disk and for transmission over the wire
void RenderablePolyVoxEntityItem::compressVolumeData() {
    int rawSize = _volData->getDepth() * _volData->getHeight() * _volData->getWidth();
    QByteArray uncompressedData = QByteArray(rawSize, '\0');

    for (int z = 0; z < _volData->getDepth(); z++) {
        for (int y = 0; y < _volData->getHeight(); y++) {
            for (int x = 0; x < _volData->getWidth(); x++) {
                uint8_t uVoxelValue = _volData->getVoxelAt(x, y, z);
                int uncompressedIndex = z * _volData->getHeight() * _volData->getWidth() + y * _volData->getWidth() + x;
                uncompressedData[uncompressedIndex] = uVoxelValue;
            }
        }
    }

    QByteArray newVoxelData = qCompress(uncompressedData, 9);
    // HACK -- until we have a way to allow for properties larger than MTU, don't update.
    if (newVoxelData.length() < 1200) {
        _voxelData = newVoxelData;
        qDebug() << "-------------- voxel compresss --------------";
        qDebug() << "raw-size =" << rawSize << "   compressed-size =" << newVoxelData.size();
    } else {
        qDebug() << "voxel data too large, reverting change.";
        // revert
        decompressVolumeData();
    }
}


// take compressed data and decompreess it into _volData.
void RenderablePolyVoxEntityItem::decompressVolumeData() {
    int rawSize = _volData->getDepth() * _volData->getHeight() * _volData->getWidth();
    QByteArray uncompressedData = QByteArray(rawSize, '\0');

    uncompressedData = qUncompress(_voxelData);

    for (int z = 0; z < _volData->getDepth(); z++) {
        for (int y = 0; y < _volData->getHeight(); y++) {
            for (int x = 0; x < _volData->getWidth(); x++) {
                int uncompressedIndex = z * _volData->getHeight() * _volData->getWidth() + y * _volData->getWidth() + x;
                _volData->setVoxelAt(x, y, z, uncompressedData[uncompressedIndex]);
            }
        }
    }

    _needsModelReload = true;

    qDebug() << "--------------- voxel decompress ---------------";
    qDebug() << "raw-size =" << rawSize << "   compressed-size =" << _voxelData.size();
}
