/*
MIT License

Copyright(c) 2018-2019 megai2

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files(the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/
#include "stdafx.h"
#include "d912pxy_upload_pool.h"

d912pxy_upload_pool::d912pxy_upload_pool() : d912pxy_pool_memcat<d912pxy_upload_item*, d912pxy_upload_pool*>()
{

}

d912pxy_upload_pool::~d912pxy_upload_pool()
{
	pRunning = 0;

	if (memPool)
		memPool->Release();

	for (int i = 0; i != PXY_INNDER_UPLOAD_POOL_BITCNT; ++i)
	{
		UINT tsz = 0;

		while (memTable[i]->HaveElements())
		{
			memTable[i]->GetElement()->Release();
			memTable[i]->Next();
			++tsz;
		}

		LOG_DBG_DTDM3("ul pool tbl[%u] = %u", i, tsz);

		delete memTable[i];		
	}


	delete ctorLock;
}

void d912pxy_upload_pool::Init()
{
	d912pxy_pool_memcat<d912pxy_upload_item*, d912pxy_upload_pool*>::Init(		
		PXY_INNDER_UPLOAD_POOL_BITIGNORE,
		PXY_INNDER_UPLOAD_POOL_BITLIMIT,
		PXY_CFG_POOLING_UPLOAD_LIMITS	
	);
}

d912pxy_upload_item * d912pxy_upload_pool::GetUploadObject(UINT size)
{
	d912pxy_upload_item * ret = NULL;
	UINT mc = MemCatFromSize(size);
	PoolRW(mc, &ret, 0);

	if (!ret)
	{
		PoolRW(mc, &ret, 1);
	}
	else {
		LOG_DBG_DTDM2("upload obj reuse %u-%u", mc, size);
		ret->PooledAction(1);
	}

	return ret;
}

d912pxy_upload_item * d912pxy_upload_pool::AllocProc(UINT32 cat)
{
	d912pxy_upload_item * ret;

	AddMemoryToPool(MemCatToSize(cat));

	ret = new d912pxy_upload_item(cat);

	return ret;
}

void d912pxy_upload_pool::EarlyInitProc()
{
	memPoolSize = d912pxy_s.config.GetValueUI64(PXY_CFG_POOLING_UPLOAD_ALLOC_STEP) << 20;
	memPool = NULL;

	if (memPoolSize)
		CreateMemPool();

	ctorLock = new d912pxy_thread_lock();
}

ID3D12Resource * d912pxy_upload_pool::MakeUploadBuffer(UINT maxSize)
{
	maxSize = MemCatToSize(maxSize);

	ID3D12Resource* ret = NULL;

	if (!memPool || (maxSize >= memPoolSize))
	{
fallback:
		d912pxy_resource* dxBuffer = new d912pxy_resource(RTID_UL_BUF, PXY_COM_OBJ_NOVTABLE, L"upload pool data");
		dxBuffer->d12res_buffer(maxSize, D3D12_HEAP_TYPE_UPLOAD);
		dxBuffer->Release();

		ret = dxBuffer->GetD12Obj();
		ret->AddRef();
	}
	else {

		const D3D12_RESOURCE_DESC rsDesc = {
			D3D12_RESOURCE_DIMENSION_BUFFER,
			0,
			maxSize,
			1,
			1,
			1,
			DXGI_FORMAT_UNKNOWN,
			{1, 0},
			D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
			D3D12_RESOURCE_FLAG_NONE
		};

		ctorLock->Hold();
		
		if (memPoolOffset + maxSize >= memPoolSize)
		{
			CreateMemPool();

			memPoolOffset = 0;
		}

		HRESULT cprHR = d912pxy_s.dx12.dev->CreatePlacedResource(
			memPool,
			memPoolOffset,
			&rsDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			0,
			IID_PPV_ARGS(&ret)
		);

		if (FAILED(cprHR))
		{
			LOG_ERR_DTDM("CreatePlacedResource failed with hr = %lX po %llX ps %llX", cprHR, memPoolOffset, memPoolSize);
			ctorLock->Release();
			goto fallback;
		}

		memPoolOffset += maxSize;

		ctorLock->Release();
	}

	return ret;
}

void d912pxy_upload_pool::CreateMemPool()
{
	if (memPool)
		memPool->Release();

	memPoolOffset = 0;

	const D3D12_HEAP_DESC heapDsc = {
		memPoolSize,
		d912pxy_s.dev.GetResourceHeap(D3D12_HEAP_TYPE_UPLOAD),
		0,
		D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS
	};

	d912pxy_s.dx12.dev->CreateHeap(
		&heapDsc,
		IID_PPV_ARGS(&memPool)
	);
}

d912pxy_upload_item::d912pxy_upload_item(UINT8 icat) : d912pxy_comhandler(PXY_COM_OBJ_NOVTABLE, L"upload item")
{
	cat = icat;	
	space = d912pxy_s.pool.upload.MemCatToSize(cat);
	usedSpace = 0;
	mRes = d912pxy_s.pool.upload.MakeUploadBuffer(cat);
	LOG_ERR_THROW2(mRes->Map(0, 0, (void**)&mappedMemWofs), "upload pool memory map error on creation");
}

d912pxy_upload_item::~d912pxy_upload_item()
{

}

void d912pxy_upload_item::UploadTargetWithOffset(ID3D12Resource * res, UINT64 sofs, UINT64 dofs, UINT64 sz, void* src, ID3D12GraphicsCommandList * cl)
{
	memcpy((void*)DPtrOffset(usedSpace), (void*)((intptr_t)src + sofs), sz);
	cl->CopyBufferRegion(res, dofs, mRes, usedSpace, sz);
	usedSpace += sz;
}

void d912pxy_upload_item::UploadTarget(ID3D12Resource * res, UINT64 dofs, UINT64 sz, void* src, ID3D12GraphicsCommandList * cl)
{
	UploadTargetWithOffset(res, 0, dofs, sz, src, cl);
}

intptr_t d912pxy_upload_item::DPtr()
{
	//d912pxy_s.dx12.dev->MakeResident(1, (ID3D12Pageable**)&mRes);

	return (mappedMemWofs);
}

intptr_t d912pxy_upload_item::DPtrOffset(UINT64 offset)
{
	return mappedMemWofs + offset;
}

void d912pxy_upload_item::Reconstruct(void* mem, UINT64 rowPitch, UINT64 height, UINT64 size, const D3D12_RANGE * wofs)
{
	intptr_t bufferRef = (intptr_t)DPtrOffset(usedSpace);
	intptr_t srcm = (intptr_t)mem;
		
	//megai2: well..
	for (int i = 0; i != height; ++i)
	{
		memcpy((void*)bufferRef, (void*)srcm, size);

		bufferRef += rowPitch;
		srcm = srcm + size;
	}

	usedSpace += rowPitch * height;

	usedSpace = d912pxy_helper::AlignValueByPow2(usedSpace, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
}

UINT d912pxy_upload_item::FinalReleaseCB()
{
	if (d912pxy_s.pool.upload.IsRunning())
	{
	//	d912pxy_s.dx12.dev->Evict(1, (ID3D12Pageable**)&mRes);

		usedSpace = 0;

		d912pxy_upload_item* tv = this;
		d912pxy_s.pool.upload.PoolRW(cat, &tv, 1);
		return 0;
	}
	else {

		if (mRes)
			mRes->Release();		
		return 1;
	}
}

UINT32 d912pxy_upload_item::PooledAction(UINT32 use)
{
	if (!d912pxy_comhandler::PooledAction(use))
		return 0;

	if (use)
	{
		mRes = d912pxy_s.pool.upload.MakeUploadBuffer(cat);

		d912pxy_s.pool.upload.AddMemoryToPool(space);

		LOG_ERR_THROW2(mRes->Map(0, 0, (void**)&mappedMemWofs), "upload pool memory map error");
	}
	else {

		d912pxy_s.pool.upload.AddMemoryToPool(-space);

		mRes->Release();
		mRes = NULL;

		mappedMemWofs = NULL;
	}


	PooledActionExit();

	return 1;
}

UINT d912pxy_upload_item::HaveFreeSpace(UINT32 size)
{
	return (space - usedSpace) > size;
}
