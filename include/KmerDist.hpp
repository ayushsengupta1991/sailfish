/**
 > HEADER                  *
 Copyright (c) 2015 Rob Patro rob.patro@cs.stonybrook.edu
 This file is part of Sailfish / Salmon.
 Sailfish is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 Sailfish is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.
 You should have received a copy of the GNU General Public License
 along with Sailfish. If not, see <http://www.gnu.org/licenses/>.
 <HEADER
 **/

#ifndef KMER_DIST_HPP__
#define KMER_DIST_HPP__
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cstdint>
// from http://stackoverflow.com/questions/17719674/c11-fast-constexpr-integer-powers
constexpr int64_t constExprPow(int64_t base, unsigned int exp, int64_t result = 1) {
	return exp < 1 ? result : constExprPow(base*base, exp/2, (exp % 2) ? result*base : result);
}
// templatized on the k-mer length
template <unsigned int K, typename CountT=uint32_t>

class KmerDist {
	using Bias = double;
	using Offset = size_t;
public:
	// The number of k-mers we'll have to store given the k-mer size
	//constexpr const uint64_t numKmers{constExprPow(4, K)};
	std::array<CountT, constExprPow(4,K)> counts;
	
	KmerDist() : haveBias_(false) {
		// set a pseudo-count for each k-mer
		for (size_t i = 0; i < counts.size(); ++i) { counts[i] = 1; }
	}
	
	inline uint32_t indexForKmer(const char* s, bool reverseComplement) {
		// The index we'll return
		uint32_t idx{0};
		// The number of bits we need to shift the
		// current mask to the left.
		uint32_t pos{0};
		if(!reverseComplement)
		{
			for (size_t i = 0; i < K; ++i) {
				char c = s[i];
				switch (c) {
					case 'A':
					case 'a':
						// don't need to do anything; or'ing with 0
						break;
					case 'C':
					case 'c':
						idx |= (0x1 << pos);
						break;
					case 'G':
					case 'g':
						idx |= (0x2 << pos);
						break;
					case 'T':
					case 't':
					case 'U':
					case 'u':
						idx |= (0x3 << pos);
						break;
					default:
						// treat anything else (e.g. an 'N' as an 'A')
						break;
				}
				pos += 2;
			}
		}
		else
		{
			for(size_t i=K-1 ; i>=0 ; i--)
			{
				switch(s[i])
				{
					case 'T':
					case 't': 
					case 'u':
					case 'U': break;
					
					case 'C':
					case 'c': idx |= (0x2 << pos);
						  break;
					
					case 'G':
					case 'g': idx |= (0x1 << pos);
						  break;
					
					case 'A': 
					case 'a': idx |= (0x3 <<pos);
						  break;
					
				}
				pos+=2;
			}
		}
		return idx;
	}
	
	inline uint32_t nextKmerIndex(uint32_t idx, char next)
	{
		idx = idx & (0x3FF);
		idx = idx << 2;
		switch(next)
		{
			case 'A':
			case 'a': break;
			
			case 'C':
			case 'c': idx = idx + 1;
				  break;
				  
			case 'G': 
			case 'g': idx = idx + 2;
				  break;
			case 'T':
			case 't': 
			case 'U':	
			case 'u':
				 idx = idx + 3;
				 break;
		}
		return idx;
	}
	
	inline std::string kmerForIndex(uint32_t idx) {
		std::string kmer(K, 'X');
		// The number of bits we need to shift the
		// current mask to the left.
		uint32_t pos{0};
		for (size_t i = 0; i < K; ++i) {
			uint8_t c = (idx >> pos) & 0x3;
			switch (c) {
				case 0:
					kmer[i] = 'A';
					break;
				case 1:
					kmer[i] = 'C';
					break;
				case 2:
					kmer[i] = 'G';
					break;
				case 3:
					kmer[i] = 'T';
					break;
				default:
					break;
			}
			pos += 2;
		}
		return kmer;
	}
	
	inline bool addKmer(const char* s, const char* end, bool reverseComplement) {
		// Make sure there are at least k bases left
		if (std::distance(s, end) >= K) {
			auto idx = indexForKmer(s,reverseComplement);
			counts[idx]++;
		} else {
			return false;
		}
	}

	private:
		bool haveBias_;
		//std::vector<Bias> biases_;
		//std::unordered_map<std::string, size_t> offsetMap_;
};
#endif // KMER_DIST_HPP__