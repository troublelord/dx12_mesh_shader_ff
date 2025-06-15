//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************
#include "stdafx.h"
#include "Model.h"

#include "DXSampleHelper.h"

#include <fstream>
#include <unordered_set>

using namespace DirectX;
using namespace Microsoft::WRL;

namespace
{ 
    uint32_t GetFormatSize(DXGI_FORMAT format)
    { 
        switch(format)
        {
            case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
            case DXGI_FORMAT_R32G32B32_FLOAT: return 12;
            case DXGI_FORMAT_R32G32_FLOAT: return 8;
            case DXGI_FORMAT_R32_FLOAT: return 4;
            default: throw std::exception("Unimplemented type");
        }
    }

    template <typename T, typename U>
    constexpr T DivRoundUp(T num, U denom)
    {
        return (num + denom - 1) / denom;
    }

    template <typename T>
    size_t GetAlignedSize(T size)
    {
        const size_t alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
        const size_t alignedSize = (size + alignment - 1) & ~(alignment - 1);
        return alignedSize;
    }
}

HRESULT Model::LoadFromMemory(
    const std::vector<MeshHeader>& meshes,
    const std::vector<Accessor>& accessors,
    const std::vector<BufferView>& bufferViews,
    const std::vector<uint8_t>& rawBuffer)
{
    m_buffer = rawBuffer;
    m_meshes.resize(meshes.size());

    for (size_t i = 0; i < meshes.size(); ++i)
    {
        const auto& meshView = meshes[i];
        auto& mesh = m_meshes[i];

        // Index data
        {
            const Accessor& accessor = accessors[meshView.Indices];
            const BufferView& bufferView = bufferViews[accessor.BufferView];

            mesh.IndexSize = accessor.Size;
            mesh.IndexCount = accessor.Count;

            mesh.Indices = MakeSpan(m_buffer.data() + bufferView.Offset, bufferView.Size);
        }

        // Vertex buffer data & layout
        std::vector<uint32_t> vbMap;
        mesh.LayoutDesc.pInputElementDescs = mesh.LayoutElems;
        mesh.LayoutDesc.NumElements = 0;

        for (uint32_t j = 0; j < Attribute::Count; ++j)
        {
            if (meshView.Attributes[j] == -1)
                continue;

            const Accessor& accessor = accessors[meshView.Attributes[j]];

            auto it = std::find(vbMap.begin(), vbMap.end(), accessor.BufferView);
            if (it != vbMap.end())
                continue;

            vbMap.push_back(accessor.BufferView);
            const BufferView& bufferView = bufferViews[accessor.BufferView];

            Span<uint8_t> verts = MakeSpan(m_buffer.data() + bufferView.Offset, bufferView.Size);
            mesh.VertexStrides.push_back(accessor.Stride);
            mesh.Vertices.push_back(verts);

            mesh.VertexCount = static_cast<uint32_t>(verts.size()) / accessor.Stride;
        }

        for (uint32_t j = 0; j < Attribute::Count; ++j)
        {
            if (meshView.Attributes[j] == -1)
                continue;

            const Accessor& accessor = accessors[meshView.Attributes[j]];
            auto it = std::find(vbMap.begin(), vbMap.end(), accessor.BufferView);

            D3D12_INPUT_ELEMENT_DESC desc = c_elementDescs[j];
            desc.InputSlot = static_cast<UINT>(std::distance(vbMap.begin(), it));
            mesh.LayoutElems[mesh.LayoutDesc.NumElements++] = desc;
        }

        // Optional: Meshlet, Culling, PrimitiveIndices, etc.
        // You can replicate those blocks from LoadFromFile if needed
    }

    // Compute bounding spheres
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_meshes.size()); ++i)
    {
        auto& m = m_meshes[i];
        uint32_t vbIndexPos = 0;

        for (uint32_t j = 1; j < m.LayoutDesc.NumElements; ++j)
        {
            auto& desc = m.LayoutElems[j];
            if (strcmp(desc.SemanticName, "POSITION") == 0)
            {
                vbIndexPos = j;
                break;
            }
        }

        uint32_t positionOffset = 0;

        for (uint32_t j = 0; j < m.LayoutDesc.NumElements; ++j)
        {
            auto& desc = m.LayoutElems[j];
            if (strcmp(desc.SemanticName, "POSITION") == 0)
                break;

            if (desc.InputSlot == vbIndexPos)
                positionOffset += GetFormatSize(m.LayoutElems[j].Format);
        }

        XMFLOAT3* v0 = reinterpret_cast<XMFLOAT3*>(m.Vertices[vbIndexPos].data() + positionOffset);
        uint32_t stride = m.VertexStrides[vbIndexPos];

        BoundingSphere::CreateFromPoints(m.BoundingSphere, m.VertexCount, v0, stride);

        if (i == 0)
        {
            m_boundingSphere = m.BoundingSphere;
        }
        else
        {
            BoundingSphere::CreateMerged(m_boundingSphere, m_boundingSphere, m.BoundingSphere);
        }
    }

    return S_OK;
}

int Model::MapSemanticToAttributeIndex(const char* semantic)
{
    if (_stricmp(semantic, "POSITION") == 0) return Attribute::Position;
    if (_stricmp(semantic, "NORMAL") == 0)   return Attribute::Normal;
    if (_stricmp(semantic, "TEXCOORD") == 0) return Attribute::TexCoord;
    if (_stricmp(semantic, "TANGENT") == 0)  return Attribute::Tangent;
    if (_stricmp(semantic, "BITANGENT") == 0) return Attribute::Bitangent;
    //if (_stricmp(semantic, "COLOR") == 0)    return Attribute::Color;

    // Unrecognized semantic
    return -1;
}

HRESULT Model::LoadFromRawBuffers(
    const std::vector<uint8_t>& vertexBuffer,
    const std::vector<uint8_t>& indexBuffer,
    uint32_t vertexStride,
    uint32_t vertexCount,
    uint32_t indexCount,
    bool use32BitIndices,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout)
{
    std::vector<MeshHeader> meshHeaders(1);
    std::vector<BufferView> bufferViews;
    std::vector<Accessor> accessors;

    std::vector<uint8_t> combinedBuffer;

    // Add vertex buffer
    uint32_t vertexOffset = static_cast<uint32_t>(combinedBuffer.size());
    combinedBuffer.insert(combinedBuffer.end(), vertexBuffer.begin(), vertexBuffer.end());

    BufferView vbView;
    vbView.Offset = vertexOffset;
    vbView.Size = static_cast<uint32_t>(vertexBuffer.size());
    bufferViews.push_back(vbView);

    Accessor vertexAccessor;
    vertexAccessor.BufferView = 0;  // Index in bufferViews
    vertexAccessor.Count = vertexCount;
    vertexAccessor.Stride = vertexStride;
    vertexAccessor.Size = static_cast<uint32_t>(vertexBuffer.size());
    accessors.push_back(vertexAccessor);

    // Add index buffer
    uint32_t indexOffset = static_cast<uint32_t>(combinedBuffer.size());
    combinedBuffer.insert(combinedBuffer.end(), indexBuffer.begin(), indexBuffer.end());

    BufferView ibView;
    ibView.Offset = indexOffset;
    ibView.Size = static_cast<uint32_t>(indexBuffer.size());
    bufferViews.push_back(ibView);

    Accessor indexAccessor;
    indexAccessor.BufferView = 1;  // Index in bufferViews
    indexAccessor.Count = indexCount;
    indexAccessor.Stride = use32BitIndices ? 4 : 2;
    indexAccessor.Size = static_cast<uint32_t>(indexBuffer.size());
    accessors.push_back(indexAccessor);

    // Prepare mesh header
    MeshHeader& mesh = meshHeaders[0];
    mesh.Indices = static_cast<uint32_t>(accessors.size() - 1); // indexAccessor index

    // Set attribute mapping (assumes inputLayout maps 1:1 with Attribute::Count)
    for (uint32_t i = 0; i < Attribute::Count; ++i)
    {
        mesh.Attributes[i] = -1;
    }

    for (uint32_t i = 0; i < static_cast<uint32_t>(inputLayout.size()); ++i)
    {
        const auto& desc = inputLayout[i];

        // Match semantic to internal Attribute enum (you must implement this mapping)
        int attrIndex = MapSemanticToAttributeIndex(desc.SemanticName); // <- You implement this
        if (attrIndex >= 0 && attrIndex < Attribute::Count)
        {
            mesh.Attributes[attrIndex] = 0; // vertexAccessor index
        }
    }

    // Fill dummy values for optional components
    mesh.IndexSubsets = -1;
    mesh.Meshlets = -1;
    mesh.MeshletSubsets = -1;
    mesh.UniqueVertexIndices = -1;
    mesh.PrimitiveIndices = -1;
    mesh.CullData = -1;

    return LoadFromMemory(meshHeaders, accessors, bufferViews, combinedBuffer);
}

HRESULT Model::LoadFromVtxBuffer(const std::vector<XMFLOAT4>& positions)
{
    //const uint32_t vertexStride = sizeof(XMFLOAT4);
    //const uint32_t vertexCount = static_cast<uint32_t>(positions.size());
    //
    //std::vector<uint8_t> vertexBuffer(vertexCount * vertexStride);
    //memcpy(vertexBuffer.data(), positions.data(), vertexBuffer.size());

    // Simulate vertex layout: POSITION only
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    m_prims.Indices.resize(positions.size());
    for (int i = 0; i < positions.size(); i++) {
        m_prims.Indices[i] = i;
    }
    m_prims.IndexCount = 3;
    m_prims.IndexSize = m_prims.Indices.size();
    
    m_prims.Vertices.resize(positions.size());
    std::copy(positions.begin(), positions.end(), m_prims.Vertices.begin());
    m_prims.VertexCount = m_prims.Vertices.size();
    

    return S_OK;
}

HRESULT Model::LoadFromFile2(const wchar_t* filename)
{
    std::ifstream stream(filename, std::ios::binary);
    if (!stream.is_open())
    {
        return E_INVALIDARG;
    }

    std::vector<MeshHeader> meshes;
    std::vector<BufferView> bufferViews;
    std::vector<Accessor> accessors;

    FileHeader header;
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (header.Prolog != c_prolog)
    {
        return E_FAIL; // Incorrect file format.
    }

    if (header.Version != CURRENT_FILE_VERSION)
    {
        return E_FAIL; // Version mismatch between export and import serialization code.
    }

    // Read mesh metdata
    meshes.resize(header.MeshCount);
    stream.read(reinterpret_cast<char*>(meshes.data()), meshes.size() * sizeof(meshes[0]));

    accessors.resize(header.AccessorCount);
    stream.read(reinterpret_cast<char*>(accessors.data()), accessors.size() * sizeof(accessors[0]));

    bufferViews.resize(header.BufferViewCount);
    stream.read(reinterpret_cast<char*>(bufferViews.data()), bufferViews.size() * sizeof(bufferViews[0]));

    m_buffer.resize(header.BufferSize);
    stream.read(reinterpret_cast<char*>(m_buffer.data()), header.BufferSize);

    char eofbyte;
    stream.read(&eofbyte, 1); // Read last byte to hit the eof bit

    assert(stream.eof()); // There's a problem if we didn't completely consume the file contents.

    stream.close();

    // Populate mesh data from binary data and metadata.
    m_meshes.resize(meshes.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(meshes.size()); ++i)
    {
        auto& meshView = meshes[i];
        auto& mesh = m_meshes[i];

        // Index data
        {
            Accessor& accessor = accessors[meshView.Indices];
            BufferView& bufferView = bufferViews[accessor.BufferView];

            mesh.IndexSize = accessor.Size;
            mesh.IndexCount = accessor.Count;

            mesh.Indices = MakeSpan(m_buffer.data() + bufferView.Offset, bufferView.Size);
        }

        // Index Subset data
        {
            Accessor& accessor = accessors[meshView.IndexSubsets];
            BufferView& bufferView = bufferViews[accessor.BufferView];

            mesh.IndexSubsets = MakeSpan(reinterpret_cast<Subset*>(m_buffer.data() + bufferView.Offset), accessor.Count);
        }

        // Vertex data & layout metadata

        // Determine the number of unique Buffer Views associated with the vertex attributes & copy vertex buffers.
        std::vector<uint32_t> vbMap;

        mesh.LayoutDesc.pInputElementDescs = mesh.LayoutElems;
        mesh.LayoutDesc.NumElements = 0;

        for (uint32_t j = 0; j < Attribute::Count; ++j)
        {
            if (meshView.Attributes[j] == -1)
                continue;

            Accessor& accessor = accessors[meshView.Attributes[j]];

            auto it = std::find(vbMap.begin(), vbMap.end(), accessor.BufferView);
            if (it != vbMap.end())
            {
                continue; // Already added - continue.
            }

            // New buffer view encountered; add to list and copy vertex data
            vbMap.push_back(accessor.BufferView);
            BufferView& bufferView = bufferViews[accessor.BufferView];

            Span<uint8_t> verts = MakeSpan(m_buffer.data() + bufferView.Offset, bufferView.Size);

            mesh.VertexStrides.push_back(accessor.Stride);
            mesh.Vertices.push_back(verts);
            mesh.VertexCount = static_cast<uint32_t>(verts.size()) / accessor.Stride;
        }

        // Populate the vertex buffer metadata from accessors.
        for (uint32_t j = 0; j < Attribute::Count; ++j)
        {
            if (meshView.Attributes[j] == -1)
                continue;

            Accessor& accessor = accessors[meshView.Attributes[j]];

            // Determine which vertex buffer index holds this attribute's data
            auto it = std::find(vbMap.begin(), vbMap.end(), accessor.BufferView);

            D3D12_INPUT_ELEMENT_DESC desc = c_elementDescs[j];
            desc.InputSlot = static_cast<uint32_t>(std::distance(vbMap.begin(), it));

            mesh.LayoutElems[mesh.LayoutDesc.NumElements++] = desc;
        }

        // Meshlet data
        {
            Accessor& accessor = accessors[meshView.Meshlets];
            BufferView& bufferView = bufferViews[accessor.BufferView];

            mesh.Meshlets = MakeSpan(reinterpret_cast<Meshlet*>(m_buffer.data() + bufferView.Offset), accessor.Count);
        }

        // Meshlet Subset data
        {
            Accessor& accessor = accessors[meshView.MeshletSubsets];
            BufferView& bufferView = bufferViews[accessor.BufferView];

            mesh.MeshletSubsets = MakeSpan(reinterpret_cast<Subset*>(m_buffer.data() + bufferView.Offset), accessor.Count);
        }

        // Unique Vertex Index data
        {
            Accessor& accessor = accessors[meshView.UniqueVertexIndices];
            BufferView& bufferView = bufferViews[accessor.BufferView];

            mesh.UniqueVertexIndices = MakeSpan(m_buffer.data() + bufferView.Offset, bufferView.Size);
        }

        // Primitive Index data
        {
            Accessor& accessor = accessors[meshView.PrimitiveIndices];
            BufferView& bufferView = bufferViews[accessor.BufferView];

            mesh.PrimitiveIndices = MakeSpan(reinterpret_cast<PackedTriangle*>(m_buffer.data() + bufferView.Offset), accessor.Count);
        }

        // Cull data
        {
            Accessor& accessor = accessors[meshView.CullData];
            BufferView& bufferView = bufferViews[accessor.BufferView];

            mesh.CullingData = MakeSpan(reinterpret_cast<CullData*>(m_buffer.data() + bufferView.Offset), accessor.Count);
        }
    }

    // Build bounding spheres for each mesh
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_meshes.size()); ++i)
    {
        auto& m = m_meshes[i];

        uint32_t vbIndexPos = 0;

        // Find the index of the vertex buffer of the position attribute
        for (uint32_t j = 1; j < m.LayoutDesc.NumElements; ++j)
        {
            auto& desc = m.LayoutElems[j];
            if (strcmp(desc.SemanticName, "POSITION") == 0)
            {
                vbIndexPos = j;
                break;
            }
        }

        // Find the byte offset of the position attribute with its vertex buffer
        uint32_t positionOffset = 0;

        for (uint32_t j = 0; j < m.LayoutDesc.NumElements; ++j)
        {
            auto& desc = m.LayoutElems[j];
            if (strcmp(desc.SemanticName, "POSITION") == 0)
            {
                break;
            }

            if (desc.InputSlot == vbIndexPos)
            {
                positionOffset += GetFormatSize(m.LayoutElems[j].Format);
            }
        }

        XMFLOAT3* v0 = reinterpret_cast<XMFLOAT3*>(m.Vertices[vbIndexPos].data() + positionOffset);
        uint32_t stride = m.VertexStrides[vbIndexPos];

        BoundingSphere::CreateFromPoints(m.BoundingSphere, m.VertexCount, v0, stride);

        if (i == 0)
        {
            m_boundingSphere = m.BoundingSphere;
        }
        else
        {
            BoundingSphere::CreateMerged(m_boundingSphere, m_boundingSphere, m.BoundingSphere);
        }
    }

    return S_OK;
}

HRESULT Model::LoadFromFile(const wchar_t* filename)
{
    std::ifstream stream(filename, std::ios::binary);
    if (!stream.is_open())
    {
        return E_INVALIDARG;
    }

    std::vector<MeshHeader> meshes;
    std::vector<BufferView> bufferViews;
    std::vector<Accessor> accessors;

    FileHeader header;
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (header.Prolog != c_prolog)
    {
        return E_FAIL; // Incorrect file format.
    }

    if (header.Version != CURRENT_FILE_VERSION)
    {
        return E_FAIL; // Version mismatch between export and import serialization code.
    }

    // Read mesh metdata
    meshes.resize(header.MeshCount);
    stream.read(reinterpret_cast<char*>(meshes.data()), meshes.size() * sizeof(meshes[0]));
    
    accessors.resize(header.AccessorCount);
    stream.read(reinterpret_cast<char*>(accessors.data()), accessors.size() * sizeof(accessors[0]));

    bufferViews.resize(header.BufferViewCount);
    stream.read(reinterpret_cast<char*>(bufferViews.data()), bufferViews.size() * sizeof(bufferViews[0]));

    m_buffer.resize(header.BufferSize);
    stream.read(reinterpret_cast<char*>(m_buffer.data()), header.BufferSize);

    char eofbyte;
    stream.read(&eofbyte, 1); // Read last byte to hit the eof bit

    assert(stream.eof()); // There's a problem if we didn't completely consume the file contents.

    stream.close();

    // Populate mesh data from binary data and metadata.
    m_meshes.resize(meshes.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(meshes.size()); ++i)
    {
        auto& meshView = meshes[i];
        auto& mesh = m_meshes[i];

        // Index data
        {
            Accessor& accessor = accessors[meshView.Indices];
            BufferView& bufferView = bufferViews[accessor.BufferView];

            mesh.IndexSize = accessor.Size;
            mesh.IndexCount = accessor.Count;

            mesh.Indices = MakeSpan(m_buffer.data() + bufferView.Offset, bufferView.Size);
        }

        // Index Subset data
        {
            Accessor& accessor = accessors[meshView.IndexSubsets];
            BufferView& bufferView = bufferViews[accessor.BufferView];

            mesh.IndexSubsets = MakeSpan(reinterpret_cast<Subset*>(m_buffer.data() + bufferView.Offset), accessor.Count);
        }

        // Vertex data & layout metadata

        // Determine the number of unique Buffer Views associated with the vertex attributes & copy vertex buffers.
        std::vector<uint32_t> vbMap;

        mesh.LayoutDesc.pInputElementDescs = mesh.LayoutElems;
        mesh.LayoutDesc.NumElements = 0;

        for (uint32_t j = 0; j < Attribute::Count; ++j)
        {
            if (meshView.Attributes[j] == -1)
                continue;

            Accessor& accessor = accessors[meshView.Attributes[j]];
            
            auto it = std::find(vbMap.begin(), vbMap.end(), accessor.BufferView);
            if (it != vbMap.end())
            {
                continue; // Already added - continue.
            }

            // New buffer view encountered; add to list and copy vertex data
            vbMap.push_back(accessor.BufferView);
            BufferView& bufferView = bufferViews[accessor.BufferView];

            Span<uint8_t> verts = MakeSpan(m_buffer.data() + bufferView.Offset, bufferView.Size);

            mesh.VertexStrides.push_back(accessor.Stride);
            mesh.Vertices.push_back(verts);
            mesh.VertexCount = static_cast<uint32_t>(verts.size()) / accessor.Stride;
        }

         // Populate the vertex buffer metadata from accessors.
        for (uint32_t j = 0; j < Attribute::Count; ++j)
        {
            if (meshView.Attributes[j] == -1)
                continue;

            Accessor& accessor = accessors[meshView.Attributes[j]];

            // Determine which vertex buffer index holds this attribute's data
            auto it = std::find(vbMap.begin(), vbMap.end(), accessor.BufferView);

            D3D12_INPUT_ELEMENT_DESC desc = c_elementDescs[j];
            desc.InputSlot = static_cast<uint32_t>(std::distance(vbMap.begin(), it));

            mesh.LayoutElems[mesh.LayoutDesc.NumElements++] = desc;
        }

        // Meshlet data
        {
            Accessor& accessor = accessors[meshView.Meshlets];
            BufferView& bufferView = bufferViews[accessor.BufferView];

            mesh.Meshlets = MakeSpan(reinterpret_cast<Meshlet*>(m_buffer.data() + bufferView.Offset), accessor.Count);
        }

        // Meshlet Subset data
        {
            Accessor& accessor = accessors[meshView.MeshletSubsets];
            BufferView& bufferView = bufferViews[accessor.BufferView];

            mesh.MeshletSubsets = MakeSpan(reinterpret_cast<Subset*>(m_buffer.data() + bufferView.Offset), accessor.Count);
        }

        // Unique Vertex Index data
        {
            Accessor& accessor = accessors[meshView.UniqueVertexIndices];
            BufferView& bufferView = bufferViews[accessor.BufferView];

            mesh.UniqueVertexIndices = MakeSpan(m_buffer.data() + bufferView.Offset, bufferView.Size);
        }

        // Primitive Index data
        {
            Accessor& accessor = accessors[meshView.PrimitiveIndices];
            BufferView& bufferView = bufferViews[accessor.BufferView];

            mesh.PrimitiveIndices = MakeSpan(reinterpret_cast<PackedTriangle*>(m_buffer.data() + bufferView.Offset), accessor.Count);
        }

        // Cull data
        {
            Accessor& accessor = accessors[meshView.CullData];
            BufferView& bufferView = bufferViews[accessor.BufferView];

            mesh.CullingData = MakeSpan(reinterpret_cast<CullData*>(m_buffer.data() + bufferView.Offset), accessor.Count);
        }
     }

    // Build bounding spheres for each mesh
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_meshes.size()); ++i)
    {
        auto& m = m_meshes[i];

        uint32_t vbIndexPos = 0;

        // Find the index of the vertex buffer of the position attribute
        for (uint32_t j = 1; j < m.LayoutDesc.NumElements; ++j)
        {
            auto& desc = m.LayoutElems[j];
            if (strcmp(desc.SemanticName, "POSITION") == 0)
            {
                vbIndexPos = j;
                break;
            }
        }

        // Find the byte offset of the position attribute with its vertex buffer
        uint32_t positionOffset = 0;

        for (uint32_t j = 0; j < m.LayoutDesc.NumElements; ++j)
        {
            auto& desc = m.LayoutElems[j];
            if (strcmp(desc.SemanticName, "POSITION") == 0)
            {
                break;
            }

            if (desc.InputSlot == vbIndexPos)
            {
                positionOffset += GetFormatSize(m.LayoutElems[j].Format);
            }
        }

        XMFLOAT3* v0 = reinterpret_cast<XMFLOAT3*>(m.Vertices[vbIndexPos].data() + positionOffset);
        uint32_t stride = m.VertexStrides[vbIndexPos];

        BoundingSphere::CreateFromPoints(m.BoundingSphere, m.VertexCount, v0, stride);

        if (i == 0)
        {
            m_boundingSphere = m.BoundingSphere;
        }
        else
        {
            BoundingSphere::CreateMerged(m_boundingSphere, m_boundingSphere, m.BoundingSphere);
        }
    }

     return S_OK;
}

HRESULT Model::UploadGpuResources(ID3D12Device* device, ID3D12CommandQueue* cmdQueue, ID3D12CommandAllocator* cmdAlloc, ID3D12GraphicsCommandList* cmdList)
{
    auto& prim = m_prims;

    // Create committed D3D resources of proper sizes
    auto indexDesc = CD3DX12_RESOURCE_DESC::Buffer(prim.Indices.size());
    
    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &indexDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&prim.IndexResource)));
    

    prim.IBView.BufferLocation = prim.IndexResource->GetGPUVirtualAddress();
    prim.IBView.Format = DXGI_FORMAT_R32_UINT;
    prim.IBView.SizeInBytes = prim.IndexCount * prim.IndexSize;

    prim.VertexResources.resize(1);
    prim.VBViews.resize(1);

    auto vertexDesc = CD3DX12_RESOURCE_DESC::Buffer(prim.Vertices.size());
    device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &vertexDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&prim.VertexResources[0]));

    prim.VBViews[0].BufferLocation = prim.VertexResources[0]->GetGPUVirtualAddress();
    prim.VBViews[0].SizeInBytes = static_cast<uint32_t>(prim.Vertices.size());
    prim.VBViews[0].StrideInBytes = sizeof(DirectX::XMFLOAT4);

    // Create upload resources
    std::vector<ComPtr<ID3D12Resource>> vertexUploads;
    ComPtr<ID3D12Resource>              indexUpload;
    
    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &indexDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&indexUpload)));
    
    // Map & copy memory to upload heap
    vertexUploads.resize(1);
    for (uint32_t j = 0; j < 1; ++j)
    {
        auto vertexDesc = CD3DX12_RESOURCE_DESC::Buffer(prim.Vertices.size());
        ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &vertexDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vertexUploads[j])));

        uint8_t* memory = nullptr;
        vertexUploads[j]->Map(0, nullptr, reinterpret_cast<void**>(&memory));
        std::memcpy(memory, prim.Vertices.data(), prim.Vertices.size());
        vertexUploads[j]->Unmap(0, nullptr);
    }

    {
        uint8_t* memory = nullptr;
        indexUpload->Map(0, nullptr, reinterpret_cast<void**>(&memory));
        std::memcpy(memory, prim.Indices.data(), prim.Indices.size());
        indexUpload->Unmap(0, nullptr);
    }

    // Populate our command list
    cmdList->Reset(cmdAlloc, nullptr);

    for (uint32_t j = 0; j < 1; ++j)
    {
        cmdList->CopyResource(prim.VertexResources[j].Get(), vertexUploads[j].Get());
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(prim.VertexResources[j].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);
    }

    D3D12_RESOURCE_BARRIER postCopyBarriers;

    cmdList->CopyResource(prim.IndexResource.Get(), indexUpload.Get());
    postCopyBarriers = CD3DX12_RESOURCE_BARRIER::Transition(prim.IndexResource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    cmdList->ResourceBarrier(1, &postCopyBarriers);

    ThrowIfFailed(cmdList->Close());

    ID3D12CommandList* ppCommandLists[] = { cmdList };
    cmdQueue->ExecuteCommandLists(1, ppCommandLists);

    // Create our sync fence
    ComPtr<ID3D12Fence> fence;
    ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));

    cmdQueue->Signal(fence.Get(), 1);

    // Wait for GPU
    if (fence->GetCompletedValue() != 1)
    {
        HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, event);

        WaitForSingleObjectEx(event, INFINITE, false);
        CloseHandle(event);
    }

    return S_OK;
}
