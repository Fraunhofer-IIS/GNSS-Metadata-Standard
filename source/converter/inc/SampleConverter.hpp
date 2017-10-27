/**
 * Author: James T. Curran
 *
 * Copyright(c) 2015 Institute of Navigation
 * http://www.ion.org
 *
 * This Metadata Converter is free software; you can redistribute it and/or
 * modify it under the terms of the Lesser GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * Lesser GNU General Public License for more details.
 *
 * You should have received a copy of the Lesser GNU General Public License
 * along with Metadata API.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "SampleFileSink.h"
#include "SampleStatisticsSink.h"

template<typename sample_base_t>
bool SampleConverter::Open(GnssMetadata::Metadata& md, std::string path_prefix)
{
	std::string fullpath;

	if (mIsOpen)
	{
		throw std::runtime_error("Error: Already open");
	}

	// find the files
	for (const GnssMetadata::File& file : md.Files())
	{
		printf("File: %s\n", file.Url().toString().c_str());
		printf("Lane: %s\n", file.Lane().Id().c_str());
	}

	LaneInterpreter* laneInterpreter;
	SampleStreamInfo commonSampleInfo;

	//////////////////////////////////////////////////////////////
	// process it as though it is a non-standard/custom format
	for (const GnssMetadata::Lane& lane : md.Lanes())
	{

		// first make sure that we can find a file for this lane
		bool foundFileForLane = false;
		std::string laneID = lane.Id();
		std::string fileURL;
		// search through files
		for (const GnssMetadata::File& file : md.Files())
		{
			if (file.Lane().Id() == laneID)
			{
				foundFileForLane = true;
				fileURL = file.Url().toString();
			}
		}
		// skip lane if we have no file
		if (!foundFileForLane)
		{
			throw std::runtime_error("SampleConverter: No file found for Lane");
		}

		// create a lane
		laneInterpreter = new LaneInterpreter(fileURL);

		// get the info that is specific to this lane, first check what it's system is
		GnssMetadata::IonString sysID = lane.Systems().front().Id();
		GnssMetadata::System system;
		GnssMetadata::AttributedObject::Search<GnssMetadata::System>(system, md, sysID);
		// assign the system frequency
		commonSampleInfo.mBaseFrequency = GnssMetadata::Frequency(system.BaseFrequency().toHertz());

		// populate it with blocks
		BlockInterpreter* blockInterp;
		for (const GnssMetadata::Block& block : lane.Blocks())
		{
			// create the block
			CreateBlockInterpreter<sample_base_t>(md, commonSampleInfo, block, &blockInterp);
			// now push the block back into the list
			laneInterpreter->AddBlock(blockInterp);
		}

		// if everything is ok, add it to the list of Lanes
		// JTC ToDo - check that it is ok!
		mLaneInterps.push_back(laneInterpreter);

		// now open the first file for each lane
		mLaneFiles[mLaneInterps.back()] = new BinaryFileSource;
		// and open the file for reading binary
		fullpath = path_prefix + mLaneInterps.back()->FileURL();
		mLaneFiles[mLaneInterps.back()]->Open(fullpath);
		if (!mLaneFiles[mLaneInterps.back()]->IsOpen())
		{
			throw std::runtime_error("SampleConverter: Could not open file");
		}
	}

	// try to add some info here

	// if this worked, then flag the converter as being open
	mIsOpen = true;

	return mIsOpen;
}

template<typename sample_base_t>
bool SampleConverter::CreateBlockInterpreter(GnssMetadata::Metadata& md, SampleStreamInfo commonSampleInfo, const GnssMetadata::Block& block,
        BlockInterpreter** blockInterp)
{

	// create a block
	*blockInterp = new BlockInterpreter(
	        static_cast<uint32_t>(block.Cycles()),
	        static_cast<uint32_t>(block.SizeHeader()),
	        static_cast<uint32_t>(block.SizeFooter())
	        );

	for (const GnssMetadata::Chunk& chunk : block.Chunks())
	{
		Chunk* chkInterp;

		// create the chunk interpreter using the md and info from chk
		switch (chunk.SizeWord())
		{
		case 1:
			CreateChunkInterpreter<uint8_t, sample_base_t>(md, commonSampleInfo, chunk, &chkInterp);
			break;
		case 2:
			CreateChunkInterpreter<uint16_t, sample_base_t>(md, commonSampleInfo, chunk, &chkInterp);
			break;
		case 4:
			CreateChunkInterpreter<uint32_t, sample_base_t>(md, commonSampleInfo, chunk, &chkInterp);
			break;
		case 8:
			CreateChunkInterpreter<uint64_t, sample_base_t>(md, commonSampleInfo, chunk, &chkInterp);
			break;
		default:
			printf("Error: unsupported Chunk::SizeWord(): %ld\n", chunk.SizeWord());
			return false;
		}

		chkInterp->SetSourceEndianness(chunk.Endian());

		// now add it to the current block
		(*blockInterp)->AddChunk(chkInterp);
	}

	//
	// ToDo: make meaningful return value
	//
	return true;
}

template<typename chunk_t, typename sample_base_t>
bool SampleConverter::CreateChunkInterpreter(GnssMetadata::Metadata& md, SampleStreamInfo commonSampleInfo, const GnssMetadata::Chunk& chunk, Chunk** chunkInterp)
{
	// create the chunk interpreter and keep a local type-specific reference here for now
	bool rightShiftWord = (chunk.Shift() == GnssMetadata::Chunk::Right);
	ChunkInterpreter<chunk_t, sample_base_t>* chunkIntrp = new ChunkInterpreter<chunk_t, sample_base_t>(static_cast<uint32_t>(chunk.CountWords()),
	        rightShiftWord);
	// assign the address
	*chunkInterp = chunkIntrp;

	uint32_t totalOccupiedBitsInChunk = 0;

	for (const GnssMetadata::Lump& lump : chunk.Lumps())
	{
		uint16_t numSampleInterpretersPerLump = 0;
		// pre-calculate the number of "called" sample interpreters required
		uint16_t numBitsInLump = 0;
		for (const GnssMetadata::IonStream& stream : lump.Streams())
		{
			numSampleInterpretersPerLump += static_cast<uint32_t>(stream.RateFactor());
			numBitsInLump += stream.Packedbits();
			//printf("Found Stream: %s\n", stream.toString().c_str());
		}

		uint16_t lnLumpRepeat = (chunk.SizeWord() * chunk.CountWords() * 8) / numBitsInLump;

		for (uint16_t lr = 0; lr < lnLumpRepeat; lr++)
		{

			uint16_t totalNumSampleInterpreters = 0;
			for (const GnssMetadata::IonStream& stream : lump.Streams())
			{

				// get or create the sample sink
				SampleSink* sampleSink = mSampleSinkFactory->GetSampleSink(stream.toString());
				SampleStreamInfo* sampleInfo = mSampleSinkFactory->GetSampleStreamInfo(stream.toString());

				uint16_t numSmpInterp = static_cast<uint32_t>(stream.RateFactor());
				uint16_t numPaddingBits = static_cast<uint32_t>(stream.Packedbits()
				        - numSmpInterp * (chunkIntrp->mSampleInterpFactory.BitWidth(stream.Format(), stream.Quantization())));

				uint16_t nextCallOrder = totalNumSampleInterpreters;
				// offset the call-order based on the shift-direction of the Lumps
				if (lump.Shift() == GnssMetadata::Lump::shiftRight)
					nextCallOrder += (lnLumpRepeat - lr) * numSampleInterpretersPerLump;
				else
					nextCallOrder += lr * numSampleInterpretersPerLump;

				if (stream.Shift() == GnssMetadata::IonStream::shiftRight)
					nextCallOrder += numSmpInterp;

				std::deque<SampleInterpreter*> streamSplIntrps;
				for (uint32_t si = 0; si < numSmpInterp; si++)
				{
					// take the templated-typed chunkInterpreter and use it to create the appropriate type of sample interpreter
					SampleInterpreter* splIntrp;
					chunkIntrp->mSampleInterpFactory.Create(sampleSink, stream.Format(), stream.Encoding(), stream.Quantization(), splIntrp, nextCallOrder);
					// and add it to the ordered list
					streamSplIntrps.push_back(splIntrp);

					if (stream.Shift() == GnssMetadata::IonStream::shiftRight)
						nextCallOrder--;
					else
						nextCallOrder++;
				}

				if (numPaddingBits > 0 && (stream.Alignment() != GnssMetadata::IonStream::Undefined))
				{
					// if necessary, make a padding-interpreter (it does nothing other than occupy space)
					SampleInterpreter* splIntrp = new SampleInterpreter(numPaddingBits, 0);
					if (stream.Alignment() == GnssMetadata::IonStream::Right)
						streamSplIntrps.push_front(splIntrp);
					else
						streamSplIntrps.push_back(splIntrp);

				}

				// now add all of the interpreters for this stream to the chunk-interpreter
				for (SampleInterpreter* i : streamSplIntrps) chunkIntrp->AddSampleInterpreter(i);
				// keep a count of the total bit-occupation of the chunk
				totalOccupiedBitsInChunk += static_cast<uint32_t>(stream.Packedbits());
				// keep track of the total number of sample interpreters
				totalNumSampleInterpreters += numSmpInterp;

				/////////////////////////////////
				// now that we have made the sample stream, gather some data about it

				// copy the common info
				*sampleInfo = commonSampleInfo;
				// assing the quantization size
				sampleInfo->mQuantization = stream.Quantization();
				// figure out the sample frequence from the rate-factor and the base frequency
				sampleInfo->mSampleFrequency = GnssMetadata::Frequency(stream.RateFactor() * sampleInfo->mBaseFrequency.toHertz());
				// identify real or complex samples (the inversion or ordering will be fixed, so you get IF or IQ from the SampleConverter.
				sampleInfo->mIsComplexSamples = (stream.Format() != GnssMetadata::IonStream::SampleFormat::IF
				                              && stream.Format() != GnssMetadata::IonStream::SampleFormat::IFn);
				// now find the band-info, check which band this stream is associated with
				GnssMetadata::IonString bandID = stream.Bands().front().Id();
				GnssMetadata::Band band;
				GnssMetadata::AttributedObject::Search<GnssMetadata::Band>(band, md, bandID);
				// copy the band info
				sampleInfo->mCenterFrequency = GnssMetadata::Frequency(band.CenterFrequency().toHertz());
				sampleInfo->mTranslatedFrequency = GnssMetadata::Frequency(band.TranslatedFrequency().toHertz());
				sampleInfo->mDelayBias = band.DelayBias();

				//
			} // end iteration over sample-streams
		} // end lump-repeat
	} // end iteration over lumps

	// now apply some padding to fill out the remainder of the chunk bits
	uint32_t numChunkPaddingBits = 8 * sizeof(chunk_t) * static_cast<uint32_t>(chunk.CountWords()) - totalOccupiedBitsInChunk;
	if (numChunkPaddingBits > 0 && (chunk.Padding() != GnssMetadata::Chunk::None))
	{
		bool front = (chunk.Padding() == GnssMetadata::Chunk::Head);
		chunkIntrp->AddSampleInterpreter(new SampleInterpreter(numChunkPaddingBits, 0), front);
	}

	//
	// ToDo: make meaningful return value
	//
	return true;
}
