#include "hash.h"

#include <iostream>

xxh64_filebuf::xxh64_filebuf(uint64_t seed)
    : myState(XXH64_createState())
{
    auto xxh64ResetResult = XXH64_reset(myState, seed);
	(void)xxh64ResetResult;
    assert(xxh64ResetResult == XXH_OK);
}

xxh64_filebuf::~xxh64_filebuf()
{
    XXH_errorcode result = XXH64_freeState(myState);
	(void)result;
    assert(result == XXH_OK);

    std::cout << "(xxh64_filebuf::~xxh64_filebuf) myTotalSizeWritten: " << myTotalSizeWritten << ", myTotalSizeRead: " << myTotalSizeRead << std::endl;
    std::cout << "(xxh64_filebuf::~xxh64_filebuf) mySyncCount: " << mySyncCount << ", myOverflowCount: " << myOverflowCount << ", myUnderflowCount: " << myUnderflowCount << std::endl;
}

int xxh64_filebuf::sync()
{
    mySyncCount++;

	return 0;
}

std::filebuf::int_type xxh64_filebuf::overflow(int_type c)
{
    myOverflowCount++;

    size_t size = epptr() - pbase();
    assert(size > 0);

    myTotalSizeWritten += size;

    auto xxh64AddResult = XXH64_update(myState, pbase(), size);
	(void)xxh64AddResult;
    assert(xxh64AddResult == XXH_OK);

    //std::cout << "(xxh64_filebuf::overflow) size: " << size << ", c: " << c << ", total size: " << myTotalSizeWritten << std::endl;

    return std::filebuf::overflow(c);
}

std::filebuf::int_type xxh64_filebuf::underflow()
{
    myUnderflowCount++;

    size_t size = egptr() - eback();
    assert(size > 0);
    assert(gptr() == egptr());

    myTotalSizeRead += size;

    auto xxh64AddResult = XXH64_update(myState, eback(), size);
	(void)xxh64AddResult;
    assert(xxh64AddResult == XXH_OK);

    //std::cout << "(xxh64_filebuf::underflow) size: " << size << ", total size: " << myTotalSizeRead << std::endl;

    return std::filebuf::underflow();
}

// std::streamsize xxh64_filebuf::xsgetn(char_type* s, std::streamsize count)
// {
//     auto size = std::filebuf::xsgetn(s, count);

//     assert(size > 0);

//     myTotalSizeRead += size;

//     auto xxh64AddResult = XXH64_update(myState, s, size);
//     assert(xxh64AddResult == XXH_OK);

//     std::cout << "(xxh64_filebuf::xsgetn) size: " << size << ", total size: " << myTotalSizeRead << std::endl;

//     return size;
// }

// std::streamsize xxh64_filebuf::xsputn(const char* s, std::streamsize n)
// {
//     auto size = std::filebuf::xsputn(s, n);

//     assert(size > 0);

//     myTotalSizeWritten += size;

//     auto xxh64AddResult = XXH64_update(myState, s, size);
//     assert(xxh64AddResult == XXH_OK);

//     std::cout << "(xxh64_filebuf::xsputn) size: " << size << ", total size: " << myTotalSizeWritten << std::endl;

//     return size;
// }

xxh64_fstream::xxh64_fstream(const std::filesystem::path& filePath, std::ios_base::openmode mode)
{
    init(&myBuffer);

    if (!myBuffer.open(filePath, mode))
        setstate(std::ios_base::failbit);

    myBuffer.pubseekoff(0, ios_base::beg);
}

/*

//#VkShaderModuleCreateInfo
// VkStructureType              sType;
//!const void*                  pNext; // todo: may contain VkShaderModuleValidationCacheCreateInfoEXT, but dont care about that for now
//+VkShaderModuleCreateFlags    flags;
//>size_t                       codeSize;
//>const uint32_t*              pCode;

static inline uint64_t hash(const VkGraphicsPipelineCreateInfo &info)
{
	XXH64_hash_t result = 0;

	//#VkGraphicsPipelineCreateInfo
	// VkStructureType                                  sType;
	// const void*                                      pNext;
	//+VkPipelineCreateFlags                            flags;
	//>uint32_t                                         stageCount;
	//>const VkPipelineShaderStageCreateInfo*           pStages;
	//>const VkPipelineVertexInputStateCreateInfo*      pVertexInputState; // todo
	//>const VkPipelineInputAssemblyStateCreateInfo*    pInputAssemblyState; // todo
	//>const VkPipelineTessellationStateCreateInfo*     pTessellationState; // todo
	//>const VkPipelineViewportStateCreateInfo*         pViewportState; // todo
	//>const VkPipelineRasterizationStateCreateInfo*    pRasterizationState; // todo
	//>const VkPipelineMultisampleStateCreateInfo*      pMultisampleState; // todo
	//>const VkPipelineDepthStencilStateCreateInfo*     pDepthStencilState;
	//>const VkPipelineColorBlendStateCreateInfo*       pColorBlendState; // todo
	//>const VkPipelineDynamicStateCreateInfo*          pDynamicState; // todo
	//+VkPipelineLayout                                 layout;
	//+VkRenderPass                                     renderPass;
	//+uint32_t                                         subpass;
	//+VkPipeline                                       basePipelineHandle;
	//+int32_t                                          basePipelineIndex;

	result = XXH64(&info.flags, sizeof(info.flags), result);

	for (uint32_t stageIt = 0; stageIt < info.stageCount; stageIt++)
	{
		const VkPipelineShaderStageCreateInfo &stage = info.pStages[stageIt];

		//#VkPipelineShaderStageCreateInfo
		// VkStructureType                     sType;
		// const void*                         pNext;
		//+VkPipelineShaderStageCreateFlags    flags;
		//+VkShaderStageFlagBits               stage;
		//!VkShaderModule                      module; // this is a pointer to a vulkan object representing the binary SPIR-V module. we replace this with a hash of the SPIR-V source.
		//>const char*                         pName;
		//>const VkSpecializationInfo*         pSpecializationInfo;

		result = XXH64(&stage.flags, sizeof(stage.flags), result);
		result = XXH64(&stage.stage, sizeof(stage.stage), result);
		// auto shaderModule = shaders.find(stage.module);
		// if (shaderModule != shaders.end())
		// 	result = XXH64(&shaderModule->shaderModuleHash, sizeof(shaderModule->shaderModuleHash), result);
		result = XXH64(stage.pName, strlen(stage.pName), result);

		if (const VkSpecializationInfo *specialization = stage.pSpecializationInfo)
		{
			//#VkSpecializationInfo
			//>uint32_t                           mapEntryCount;
			//>const VkSpecializationMapEntry*    pMapEntries;
			// size_t                             dataSize;
			// const void*                        pData;

			for (uint32_t mapEntryIt = 0; mapEntryIt < specialization->mapEntryCount; mapEntryIt++)
			{
				const VkSpecializationMapEntry &mapEntry = specialization->pMapEntries[mapEntryIt];

				//#VkSpecializationMapEntry
				//+uint32_t    constantID;
				//+uint32_t    offset;
				//+size_t      size;

				result = XXH64(&mapEntry, sizeof(mapEntry), result);
			}
		}
	}

	if (const VkPipelineDepthStencilStateCreateInfo *depthStencilStatePtr = info.pDepthStencilState)
	{
		const VkPipelineDepthStencilStateCreateInfo &depthStencilState = *depthStencilStatePtr;

		//#VkPipelineDepthStencilStateCreateInfo
		// VkStructureType                           sType;
		// const void*                               pNext;
		//+VkPipelineDepthStencilStateCreateFlags    flags;
		//+VkBool32                                  depthTestEnable;
		//+VkBool32                                  depthWriteEnable;
		//+VkCompareOp                               depthCompareOp;
		//+VkBool32                                  depthBoundsTestEnable;
		//+VkBool32                                  stencilTestEnable;
		//+VkStencilOpState                          front;
		//+VkStencilOpState                          back;
		//+float                                     minDepthBounds;
		//+float                                     maxDepthBounds;

		result = XXH64(&depthStencilState.flags, sizeof(depthStencilState) - offsetof(VkPipelineDepthStencilStateCreateInfo, flags), result);
	}

	result = XXH64(&info.layout, sizeof(info) - offsetof(VkGraphicsPipelineCreateInfo, layout), result);

	return result;
}
*/