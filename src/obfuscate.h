#ifndef obfuscate_h_included
#define obfuscate_h_included

//Usage: 
//  1. Use OBF?() throughout your code to indicate the need for obfuscation
//  1a. OBFX() roughly means 'add no more than 10^(X/2) CPU cycles for obfuscation'
//      i.e. OBF2() adds up to 10 CPU cycles, OBF3() - up to 30 CPU cycles, 
//           and OBF5() - up to 300 CPU cycles
//  2. compile your code without -DOBFUSCATE_SEED for debugging and during development
//  3. compile with -DOBFUSCATE_SEED=0x<really-random-64-bit-seed>u64 for deployments (MSVC)
//  3a. GCC is not supported (yet)

#include <stdint.h>
#include <inttypes.h>
#include <array>
#include <assert.h>
#include <type_traits>
#include <atomic>//for OBF_STRICT_MT
#include <string>//for dbgPrint() only
#include <iostream>//for dbgPrint() only

#ifdef OBF_INTERNAL_DBG // set of settings currently used for internal testing. DON'T rely on it!
//#define OBFUSCATE_DEBUG_ENABLE_DBGPRINT
//#if 0

#define OBFUSCATE_SEED 0x0c7dfa61a867b125ui64 //example for MSVC
#define OBFUSCATE_INIT 
	//enables rather nasty obfuscations (including PEB-based debugger detection),
	//  but requires you to call obf_init() BEFORE ANY obf<> objects are used. 
	//  As a result - it can backfire for obfuscations-used-from-global-constructors :-(.

#define OBF_STRICT_MT//strict multithreading. Short story: as of now, more recommended than not.
	//Long story: in practice, with proper alignments should be MT-safe even without it, 
	//      but is formally UB, so future compilers MAY generate code which will fail under heavy MT (hard to imagine, but...) :-( 
	//      OTOH, at least for x64 generates not-too-obvious XCHG instruction (with implicit LOCK completely missed by current IDA decompiler)

//#define OBFUSCATE_DEBUG_DISABLE_ANTI_DEBUG
	//disables built-in anti-debugger kinda-protections

#define OBF_COMPILE_TIME_TESTS 100//supposed to affect ONLY time of compilation

//THE FOLLOWING MUST BE NOT USED FOR PRODUCTION BUILDS:
#define OBFUSCATE_DEBUG_ENABLE_DBGPRINT
	//enables dbgPrint()
#endif//OBF_INTERNAL_DBG

#ifdef _MSC_VER
#pragma warning (disable:4307)
#define FORCEINLINE __forceinline
#define NOINLINE __declspec(noinline)
#else
#error non-MSVC compilers are not supported (yet?)
#endif

#ifdef OBFUSCATE_SEED
	using OBFSEED = uint64_t;
	using OBFCYCLES = int32_t;//signed!

#ifndef OBFSCALE//#define for libraries, using OBFN() macros; 
				//  allows to promote/demote obfuscation scale of the whole library by OBFSCALE
				//  OBFSCALE = 1 'converts' all OBF0()'s into OBF1()'s, and so on...
				//  DOES NOT affect obfN<> without macros(!)
#define OBFSCALE 0
#endif

#ifndef OBF_WEIGHTLARGECONST 
#define OBF_WEIGHTLARGECONST 0//currently RECOMMENDED 0; large constants tend to act as "signatures" within the code, so finding reverse one becomes easy...
#endif
#ifndef OBF_WEIGHTSMALLCONST 
#define OBF_WEIGHTSMALLCONST 100
#endif
#ifndef OBF_WEIGHTSPECIALCONST 
#define OBF_WEIGHTSPECIALCONST 100
#endif
	namespace obf {

//POTENTIALLY user-modifiable constexpr function:
constexpr OBFCYCLES obf_exp_cycles(int exp) {
	if (exp < 0)
		return 0;
	OBFCYCLES ret = 1;
	if (exp & 1) {
		ret *= 3;
		exp -= 1;
	}
	assert((exp & 1) == 0);
	exp >>= 1;
	for (int i = 0; i < exp; ++i)
		ret *= 10;
	return ret;
}

//helper constexpr functions
	constexpr OBFSEED obf_compile_time_prng(OBFSEED seed, int iteration) {
		static_assert(sizeof(OBFSEED) == 8);
		assert(iteration > 0);
		OBFSEED ret = seed;
		for (int i = 0; i < iteration; ++i) {
			ret = UINT64_C(6364136223846793005) * ret + UINT64_C(1442695040888963407);//linear congruential one; TODO: replace with something crypto-strength for production use
		}
		return ret;
	}

	constexpr OBFSEED obf_seed_from_file_line(const char* file, int line) {
		OBFSEED ret = OBFUSCATE_SEED ^ line;
		for (const char* p = file; *p; ++p)//effectively djb2 by Dan Bernstein, albeit with different initializer
			ret = ((ret << 5) + ret) + *p;
		return obf_compile_time_prng(ret,1);//to reduce ill effects from a low-quality PRNG
	}

	template<class T, size_t N>
	constexpr T obf_compile_time_approximation(T x, std::array<T,N> xref, std::array<T,N> yref) {
		for (size_t i = 0; i < N-1; ++i) {
			T x0 = xref[i];
			T x1 = xref[i + 1];
			if (x >= x0 && x < x1) {
				T y0 = yref[i];
				T y1 = yref[i + 1];
				return uint64_t(y0 + double(x - x0)*double(y1 - y0) / double(x1 - x0));
			}
		}
	}

	constexpr uint64_t obf_sqrt_very_rough_approximation(uint64_t x0) {
		std::array<uint64_t,64> xref = {};
		std::array<uint64_t, 64> yref = {};
		for (size_t i = 1; i < 64; ++i) {
			uint64_t x = UINT64_C(1) << (i-1);
			xref[i] = x * x;
			yref[i] = x;
		}
		return obf_compile_time_approximation(x0, xref, yref);
	}

	template<class MAX>
	constexpr MAX obf_weak_random(OBFSEED seed, MAX n) {
		return seed % n;//slightly biased, but for our purposes will do
	}

	template<size_t N>
	constexpr size_t obf_random_from_list(OBFSEED seed, std::array<size_t, N> weights) {
		//returns index in weights
		size_t totalWeight = 0;
		for (size_t i = 0; i < weights.size(); ++i)
			totalWeight += weights[i];
		size_t refWeight = obf_weak_random(seed, totalWeight);
		assert(refWeight < totalWeight);
		for (size_t i = 0; i < weights.size(); ++i) {
			if (refWeight < weights[i])
				return i;
			refWeight -= weights[i];
		}
		assert(false);
	}

	struct ObfDescriptor {
		bool is_recursive;
		OBFCYCLES min_cycles;
		size_t weight;

		constexpr ObfDescriptor(bool is_recursive_, OBFCYCLES min_cycles_, size_t weight_)
			: is_recursive(is_recursive_), min_cycles(min_cycles_), weight(weight_) {
		}
	};

	template<size_t N>
	constexpr size_t obf_random_obf_from_list(OBFSEED seed, OBFCYCLES cycles, std::array<ObfDescriptor, N> descr) {
		//returns index in descr
		size_t sz = descr.size();
		std::array<size_t, N> nr_weights = {};
		std::array<size_t, N> r_weights = {};
		size_t sum_r = 0;
		size_t sum_nr = 0;
		for (size_t i = 0; i < sz; ++i) {
			if (cycles >= descr[i].min_cycles)
				if (descr[i].is_recursive) {
					r_weights[i] = descr[i].weight;
					sum_r += r_weights[i];
				}
				else {
					nr_weights[i] = descr[i].weight;
					sum_nr += nr_weights[i];
				}
		}
		if (sum_r)
			return obf_random_from_list(seed, r_weights);
		else {
			assert(sum_nr > 0);
			return obf_random_from_list(seed, nr_weights);
		}
	}

	template<class T>
	constexpr T obf_gen_const(OBFSEED seed) {
		static_assert(std::is_integral<T>::value);
		static_assert(sizeof(OBFSEED) >= sizeof(T));
		constexpr std::array<size_t, 3> weights = { OBF_WEIGHTLARGECONST , OBF_WEIGHTSMALLCONST, OBF_WEIGHTSPECIALCONST };
		size_t idx = obf_random_from_list(obf_compile_time_prng(seed, 1), weights);
		if (idx == 0)//large const
			return (T)obf_compile_time_prng(seed, 2);
		else if (idx == 1)//small const
			return ((T)obf_compile_time_prng(seed, 2)) & 0xFF;
		else {//special const
			assert(idx == 2);
			const std::array<T, 31> specials{ 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 16, 20, 24, 25, 30, 31, 32, 48, 64, 80, 96, 100, 120, 144, 160, 200, 240, 255 };
			size_t sIdx = obf_weak_random(obf_compile_time_prng(seed, 2), specials.size());
			return specials[sIdx];
		}
		return 0;
	}

	//type helpers
	//obf_half_size_int<>
	template<class T>
	struct obf_half_size_int;

	template<>
	struct obf_half_size_int<uint16_t> {
		using value_type = uint8_t;
	};

	template<>
	struct obf_half_size_int<int16_t> {
		using value_type = int8_t;
	};

	template<>
	struct obf_half_size_int<uint32_t> {
		using value_type = uint16_t;
	};

	template<>
	struct obf_half_size_int<int32_t> {
		using value_type = int16_t;
	};

	template<>
	struct obf_half_size_int<uint64_t> {
		using value_type = uint32_t;
	};

	template<>
	struct obf_half_size_int<int64_t> {
		using value_type = int32_t;
	};

	//forward declarations
	template<class T, class Context, OBFSEED seed, OBFCYCLES cycles>
	class obf_injection;
	template<class T_, T_ C_, OBFSEED seed, OBFCYCLES cycles>
	class obf_literal;

#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
	//dbgPrint helpers
	template<class T>
	std::string obf_dbgPrintT() {
		return std::string("T(sizeof=")+std::to_string(sizeof(T))+")";
	}
#endif

	//ObfRecursiveContext
	template<class T, class Context, OBFSEED seed,OBFCYCLES cycles>
	struct ObfRecursiveContext;
	
	//injection-with-constant - building block both for injections and for literals
	template <size_t which, class T, class Context, OBFSEED seed, OBFCYCLES cycles>
	class obf_injection_version;

	//version 0: identity
	struct obf_injection_version0_descr {
		//cannot make it a part of class obf_injection_version<0, T, C, seed, cycles>,
		//  as it would cause infinite recursion in template instantiation
		static constexpr ObfDescriptor descr = ObfDescriptor(false, 0, 1);
	};

	template <class T, class Context, OBFSEED seed, OBFCYCLES cycles>
	class obf_injection_version<0, T, Context, seed, cycles> {
		static_assert(std::is_integral<T>::value);
		static_assert(std::is_unsigned<T>::value);
	public:
		using return_type = T;
		FORCEINLINE constexpr static return_type injection(T x) {
			return Context::final_injection(x);
		}
		FORCEINLINE constexpr static T surjection(return_type y) {
			return Context::final_surjection(y);
		}

#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
		static void dbgPrint(size_t offset = 0) {
			std::cout << std::string(offset, ' ') << "obf_injection_version<0/*identity*/," << obf_dbgPrintT<T>() << "," << seed << "," << cycles << ">" << std::endl;
			Context::dbgPrint(offset + 1);
		}
#endif
	};

	//version 1: add mod 2^n
	struct obf_injection_version1_descr {
		static constexpr ObfDescriptor descr = ObfDescriptor(true, 1, 100);
	};

	template <class T, class Context, OBFSEED seed, OBFCYCLES cycles>
	class obf_injection_version<1, T, Context, seed, cycles> {
		static_assert(std::is_integral<T>::value);
		static_assert(std::is_unsigned<T>::value);
	public:
		using RecursiveInjection = obf_injection<T, Context, obf_compile_time_prng(seed, 1), cycles - 1>;
		using return_type = typename RecursiveInjection::return_type;
		constexpr static T C = obf_gen_const<T>(obf_compile_time_prng(seed, 2));
		FORCEINLINE constexpr static return_type injection(T x) {
			return RecursiveInjection::injection(x + C);
		}
		FORCEINLINE constexpr static T surjection(return_type y) {
			return RecursiveInjection::surjection(y) - C;
		}

#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
		static void dbgPrint(size_t offset = 0) {
			std::cout << std::string(offset, ' ') << "obf_injection_version<1/*add mod 2^N*/,"<<obf_dbgPrintT<T>() <<"," << seed << "," << cycles << ">: C=" << C << std::endl;
			RecursiveInjection::dbgPrint(offset + 1);
		}
#endif
	};

	//version 2: kinda-Feistel round
	template<class T>
	struct obf_injection_version2_descr {
		static constexpr ObfDescriptor descr =
			sizeof(T) > 1 ?
			ObfDescriptor(true, 20, 100) :
			ObfDescriptor(false, 0, 0);
	};

	template <class T, class Context, OBFSEED seed, OBFCYCLES cycles>
	class obf_injection_version<2, T, Context, seed, cycles> {
		static_assert(std::is_integral<T>::value);
		static_assert(std::is_unsigned<T>::value);
	public:
		using RecursiveInjection = obf_injection<T, Context, obf_compile_time_prng(seed, 1), cycles - 15>;
		using return_type = typename RecursiveInjection::return_type;
		using halfT = typename obf_half_size_int<T>::value_type;
		constexpr static int halfTBits = sizeof(halfT) * 8;
		constexpr static T mask = ((T)1 << halfTBits) - 1;
		FORCEINLINE constexpr static return_type injection(T x) {
			T lo = x >> halfTBits;
			T hi = (x & mask) + f((halfT)lo);
			return RecursiveInjection::injection((hi << halfTBits) + lo);
		}
		FORCEINLINE constexpr static T surjection(return_type y_) {
			auto y = RecursiveInjection::surjection(y_);
			T hi = y >> halfTBits;
			T lo = y;
			T z = (hi - f((halfT)lo)) & mask;
			return z + (lo << halfTBits);
		}

#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
		static void dbgPrint(size_t offset = 0) {
			std::cout << std::string(offset, ' ') << "obf_injection_version<2/*kinda-Feistel*/,"<<obf_dbgPrintT<T>()<<"," << seed << "," << cycles << ">" << std::endl;
			RecursiveInjection::dbgPrint(offset + 1);
		}
#endif

	private:
		static constexpr halfT f(halfT x) {
			constexpr auto func = obf_weak_random(obf_compile_time_prng(seed, 2), 3);
			//TODO: change to obf_random_obf_from_list, including injections
			if constexpr(func == 0) {
				return x*x;
			}
			else if constexpr(func == 1) {
				constexpr halfT a = (halfT)obf_compile_time_prng(seed, 3);
				constexpr halfT c = (halfT)obf_compile_time_prng(seed, 4);
				return x * a + c;
			}
			else if constexpr(func == 2) {
				constexpr halfT c = (halfT)obf_compile_time_prng(seed, 4);
				return x + c;
			}
			else {
				assert(false);
			}
		}
	};

	//version 3: split-join
	template<class T>
	struct obf_injection_version3_descr {
		static constexpr ObfDescriptor descr =
			sizeof(T) > 1 ?
			ObfDescriptor(true, 15, 100) :
			ObfDescriptor(false, 0, 0);
	};

	template <class T, class Context, OBFSEED seed, OBFCYCLES cycles>
	class obf_injection_version<3, T, Context, seed, cycles> {
		static_assert(std::is_integral<T>::value);
		static_assert(std::is_unsigned<T>::value);
	public:
		//TODO:split-join based on union
		using halfT = typename obf_half_size_int<T>::value_type;
		constexpr static int halfTBits = sizeof(halfT) * 8;

		static constexpr OBFCYCLES RemainingCycles1 = cycles - 5;
		static constexpr OBFCYCLES RecursiveCycles = obf_weak_random(obf_compile_time_prng(seed, 1), RemainingCycles1 - 5);
		using RecursiveInjection = obf_injection<T, Context, obf_compile_time_prng(seed, 2), RecursiveCycles>;
		using return_type = typename RecursiveInjection::return_type;

		static constexpr OBFCYCLES RemainingCycles2 = RemainingCycles1 - RecursiveCycles;
		static constexpr OBFCYCLES HiCycles = obf_weak_random(obf_compile_time_prng(seed, 3), RemainingCycles2 - 2);
		using HiInjection = obf_injection<halfT, typename ObfRecursiveContext<halfT, Context, obf_compile_time_prng(seed, 4),10/*TODO*/>::side_context_type/*ObfZeroContext<halfT>*/, obf_compile_time_prng(seed, 5), HiCycles>;
		static constexpr OBFCYCLES LoCycles = RemainingCycles2 - HiCycles;
		using LoInjection = obf_injection<halfT, typename ObfRecursiveContext<halfT, Context, obf_compile_time_prng(seed, 6), 10/*TODO*/>::side_context_type/*ObfZeroContext<halfT>*/, obf_compile_time_prng(seed, 7), LoCycles>;
		static_assert(sizeof(HiInjection::return_type) == sizeof(halfT));//bijections ONLY; TODO: enforce
		static_assert(sizeof(LoInjection::return_type) == sizeof(halfT));//bijections ONLY; TODO: enforce

		FORCEINLINE constexpr static return_type injection(T x) {
			halfT lo = x >> halfTBits;
			typename LoInjection::return_type lo1 = LoInjection::injection(lo);
			lo = *reinterpret_cast<halfT*>(&lo1);//relies on static_assert(sizeof(return_type)==sizeof(halfT)) above
			halfT hi = (halfT)x;
			typename HiInjection::return_type hi1 = HiInjection::injection(hi);
			hi = *reinterpret_cast<halfT*>(&hi1);//relies on static_assert(sizeof(return_type)==sizeof(halfT)) above
			return RecursiveInjection::injection((T(hi) << halfTBits) + T(lo));
		}
		FORCEINLINE constexpr static T surjection(return_type y_) {
			auto y = RecursiveInjection::surjection(y_);
			halfT hi = y >> halfTBits;
			halfT lo = (halfT)y;
			hi = HiInjection::surjection(*reinterpret_cast<typename HiInjection::return_type*>(&hi));//relies on static_assert(sizeof(return_type)==sizeof(halfT)) above
			lo = LoInjection::surjection(*reinterpret_cast<typename LoInjection::return_type*>(&lo));//relies on static_assert(sizeof(return_type)==sizeof(halfT)) above
			return T(hi) + (T(lo) << halfTBits);
		}

#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
		static void dbgPrint(size_t offset = 0) {
			std::cout << std::string(offset, ' ') << "obf_injection_version<3/*split-join*/,"<<obf_dbgPrintT<T>()<<"," << seed << "," << cycles << ">" << std::endl;
			std::cout << std::string(offset, ' ') << "Lo: " << std::endl;
			LoInjection::dbgPrint(offset + 1);
			std::cout << std::string(offset, ' ') << "Hi: " << std::endl;
			HiInjection::dbgPrint(offset + 1);
			std::cout << std::string(offset, ' ') << "Recursive: " << std::endl;
			RecursiveInjection::dbgPrint(offset + 1);
		}
#endif
	};
	
	//version 4: multiply by odd
	template< class T >
	constexpr T obf_mul_inverse_mod2n(T num) {//extended GCD, intended to be used in compile-time only
											  //by Dmytro Ivanchykhin
		assert(num & 1);
		T num0 = num;
		T x = 0, lastx = 1, y = 1, lasty = 0;
		T q=0, temp1=0, temp2=0, temp3=0;
		T mod = 0;

		// zero step: do some tricks to avoid overflowing
		// note that initially mod is power of 2 that does not fit to T
		if (num == T(mod - T(1)))
			return num;
		q = T((T(mod - num)) / num) + T(1);
		temp1 = (T(T(T(mod - T(2))) % num) + T(2)) % num;
		mod = num;
		num = temp1;

		temp2 = x;
		x = lastx - T(q * x);
		lastx = temp2;

		temp3 = y;
		y = lasty - T(q * y);
		lasty = temp3;

		while (num != 0) {
			q = mod / num;
			temp1 = mod % num;
			mod = num;
			num = temp1;

			temp2 = x;
			x = lastx - T(q * x);
			lastx = temp2;

			temp3 = y;
			y = lasty - T(q * y);
			lasty = temp3;
		}
		assert(T(num0*lasty) == T(1));
		return lasty;
	}

	struct obf_injection_version4_descr {
		static constexpr ObfDescriptor descr = ObfDescriptor(true, 10, 100);
	};

	template <class T, class Context, OBFSEED seed, OBFCYCLES cycles>
	class obf_injection_version<4, T, Context, seed, cycles> {
		static_assert(std::is_integral<T>::value);
		static_assert(std::is_unsigned<T>::value);
	public:
		using RecursiveInjection = obf_injection<T, Context, obf_compile_time_prng(seed, 1), cycles - 3>;
		using return_type = typename RecursiveInjection::return_type;
		constexpr static T C = (T)(obf_gen_const<T>(obf_compile_time_prng(seed, 2)) | 1);
		constexpr static T CINV = obf_mul_inverse_mod2n(C);
		static_assert((T)(C*CINV) == (T)1);

		FORCEINLINE constexpr static return_type injection(T x) {
			return RecursiveInjection::injection(x * obf_literal<T,CINV, obf_compile_time_prng(seed, 3),std::min(10,cycles-3)>().value());//using CINV in injection to hide literals a bit better...
		}
		FORCEINLINE constexpr static T surjection(return_type y) {
			return RecursiveInjection::surjection(y) * C;
		}

#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
		static void dbgPrint(size_t offset = 0) {
			std::cout << std::string(offset, ' ') << "obf_injection_version<4/*mul odd mod 2^N*/,"<<obf_dbgPrintT<T>()<<"," << seed << "," << cycles << ">: C=" << C << " CINV=" << CINV << std::endl;
			RecursiveInjection::dbgPrint(offset + 1);
		}
#endif
	};

	//version 5: split (w/o join)
	template<class T>
	struct obf_injection_version5_descr {
		static constexpr ObfDescriptor descr =
			sizeof(T) > 1 ?
			ObfDescriptor(true, 15, 100) :
			ObfDescriptor(false, 0, 0);
	};

	template <class T, class Context, OBFSEED seed, OBFCYCLES cycles>
	class obf_injection_version<5, T, Context, seed, cycles> {
		static_assert(std::is_integral<T>::value);
		static_assert(std::is_unsigned<T>::value);
	public:
		using halfT = typename obf_half_size_int<T>::value_type;
		constexpr static int halfTBits = sizeof(halfT) * 8;

		static constexpr OBFCYCLES RemainingCycles = cycles - 5;
		static constexpr OBFCYCLES HiCycles = obf_weak_random(obf_compile_time_prng(seed, 3), RemainingCycles - 2);
		using RecursiveInjectionHi = obf_injection<halfT, typename ObfRecursiveContext<halfT,Context, obf_compile_time_prng(seed, 4),10/*TODO*/>::recursive_context_type/*ObfZeroContext<halfT>*/, obf_compile_time_prng(seed, 5), HiCycles>;
		static constexpr OBFCYCLES LoCycles = RemainingCycles - HiCycles;
		using RecursiveInjectionLo = obf_injection<halfT, typename ObfRecursiveContext<halfT,Context, obf_compile_time_prng(seed, 6),10/*TODO*/>::recursive_context_type/*ObfZeroContext<halfT>*/, obf_compile_time_prng(seed, 7), LoCycles>;

		struct return_type {
			typename RecursiveInjectionLo::return_type lo;
			typename RecursiveInjectionHi::return_type hi;
		};
		FORCEINLINE constexpr static return_type injection(T x) {
			return_type ret{ RecursiveInjectionLo::injection((halfT)x), RecursiveInjectionHi::injection(x >> halfTBits) };
			return ret;
		}
		FORCEINLINE constexpr static T surjection(return_type y_) {
			halfT hi = RecursiveInjectionHi::surjection(y_.hi);
			halfT lo = RecursiveInjectionLo::surjection(y_.lo);
			return (T)lo + ((T)hi << halfTBits);
		}

#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
		static void dbgPrint(size_t offset = 0) {
			std::cout << std::string(offset, ' ') << "obf_injection_version<5/*split*/,"<<obf_dbgPrintT<T>()<<"," << seed << "," << cycles << ">" << std::endl;
			std::cout << std::string(offset, ' ') << "Lo: " << std::endl;
			RecursiveInjectionLo::dbgPrint(offset + 1);
			std::cout << std::string(offset, ' ') << "Hi: " << std::endl;
			RecursiveInjectionHi::dbgPrint(offset + 1);
		}
#endif
	};
	//obf_injection: combining obf_injection_version
	template<class T, class Context, OBFSEED seed, OBFCYCLES cycles>
	class obf_injection {
		static_assert(std::is_integral<T>::value);
		static_assert(std::is_unsigned<T>::value);
		constexpr static std::array<ObfDescriptor, 6> descr{
			obf_injection_version0_descr::descr,
			obf_injection_version1_descr::descr,
			obf_injection_version2_descr<T>::descr,
			obf_injection_version3_descr<T>::descr,
			obf_injection_version4_descr::descr,
			obf_injection_version5_descr<T>::descr,
		};
		constexpr static size_t which = obf_random_obf_from_list(obf_compile_time_prng(seed, 1), cycles, descr);
		using WhichType = obf_injection_version<which, T, Context, seed, cycles>;

	public:
		using return_type = typename WhichType::return_type;
		FORCEINLINE constexpr static return_type injection(T x) {
			return WhichType::injection(x);
		}
		FORCEINLINE constexpr static T surjection(return_type y) {
			return WhichType::surjection(y);
		}

#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
		static void dbgPrint(size_t offset = 0) {
			size_t dbgWhich = obf_random_obf_from_list(obf_compile_time_prng(seed, 1), cycles, descr);
			std::cout << std::string(offset, ' ') << "obf_injection<"<<obf_dbgPrintT<T>()<<"," << seed << "," << cycles << ">: which=" << which << " dbgWhich=" << dbgWhich << std::endl;
			WhichType::dbgPrint(offset + 1);
		}
#endif
	};

	//ObfLiteralContext
	template<size_t which, class T, OBFSEED seed>
	struct ObfLiteralContext_version;
	//forward declaration:
	template<class T, OBFSEED seed, OBFCYCLES cycles>
	class ObfLiteralContext;

	//version 0: identity
	struct obf_literal_context_version0_descr {
		static constexpr ObfDescriptor descr = ObfDescriptor(false, 0, 1);
	};

	template<class T,OBFSEED seed>
	struct ObfLiteralContext_version<0,T,seed> {
		static_assert(std::is_integral<T>::value);
		static_assert(std::is_unsigned<T>::value);
		static constexpr T final_injection(T x) {
			return x;
		}
		static constexpr T final_surjection(T y) {
			return y;
		}

#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
		static void dbgPrint(size_t offset = 0) {
			std::cout << std::string(offset, ' ') << "ObfLiteralContext_version<0/*identity*/," << obf_dbgPrintT<T>() << ">" << std::endl;
		}
#endif
	};

	//version 1: global volatile constant
	struct obf_literal_context_version1_descr {
		static constexpr ObfDescriptor descr = ObfDescriptor(true, 6, 100);
	};

	template<class T, OBFSEED seed>
	struct ObfLiteralContext_version<1,T,seed> {
		static_assert(std::is_integral<T>::value);
		static_assert(std::is_unsigned<T>::value);
		static constexpr T CC = obf_gen_const<T>(obf_compile_time_prng(seed, 1));
		static constexpr T final_injection(T x) {
			return x + CC;
		}
		static T final_surjection(T y) {
			return y - c;
		}

#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
		static void dbgPrint(size_t offset = 0) {
			std::cout << std::string(offset, ' ') << "ObfLiteralContext_version<1/*global volatile*/," << obf_dbgPrintT<T>() << "," << seed << ">: CC=" << CC << std::endl;
		}
#endif
	private:
		static volatile T c;
	};

	template<class T, OBFSEED seed>
	volatile T ObfLiteralContext_version<1, T, seed>::c = CC;

	//version 2: aliased pointers
	struct obf_literal_context_version2_descr {
		static constexpr ObfDescriptor descr = ObfDescriptor(true, 10, 100);
	};

	template<class T>//TODO: randomize contents of the function
	NOINLINE T obf_aliased_zero(T* x, T* y) {
		*x = 0;
		*y = 1;
		return *x;
	}

	template<class T, OBFSEED seed>
	struct ObfLiteralContext_version<2,T,seed> {
		static_assert(std::is_integral<T>::value);
		static_assert(std::is_unsigned<T>::value);
		static constexpr T final_injection(T x) {
			return x;
		}
		static /*non-constexpr*/ T final_surjection(T y) {
			T x, yy;
			T z = obf_aliased_zero(&x, &yy);
			return y - z;
		}
#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
		static void dbgPrint(size_t offset = 0) {
			std::cout << std::string(offset, ' ') << "ObfLiteralContext_version<2/*func with aliased pointers*/," << obf_dbgPrintT<T>() << "," << seed << ">:" << std::endl;
		}
#endif
	};

	//version 3: Windows/PEB
	struct obf_literal_context_version3_descr {
#ifdef _MSC_VER
		static constexpr ObfDescriptor descr = ObfDescriptor(true, 10, 100);
#else
		static constexpr ObfDescriptor descr = ObfDescriptor(false, 0, 0);
#endif
	};

#ifdef _MSC_VER
	extern volatile uint8_t* obf_peb;

	template<class T, OBFSEED seed>
	struct ObfLiteralContext_version<3,T,seed> {
		static_assert(std::is_integral<T>::value);
		static_assert(std::is_unsigned<T>::value);
		static constexpr T CC = obf_gen_const<T>(obf_compile_time_prng(seed, 1));
		static constexpr T final_injection(T x) {
			return x + CC;
		}
		static T final_surjection(T y) {
#ifdef OBFUSCATE_DEBUG_DISABLE_ANTI_DEBUG
			return y - CC;
#else
			return y - CC * (1 + obf_peb[2]);
#endif
		}

#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
		static void dbgPrint(size_t offset = 0) {
			std::cout << std::string(offset, ' ') << "ObfLiteralContext_version<3/*PEB*/," << obf_dbgPrintT<T>() << "," << seed << ">: CC=" << CC << std::endl;
		}
#endif
	};
#endif

	//version 4: global var-with-invariant
	struct obf_literal_context_version4_descr {
		static constexpr ObfDescriptor descr = ObfDescriptor(true, 10, 100);
	};

	template<class T, OBFSEED seed>
	struct ObfLiteralContext_version<4, T, seed> {
		static_assert(std::is_integral<T>::value);
		static_assert(std::is_unsigned<T>::value);
		static constexpr T PREMODRNDCONST = obf_gen_const<T>(obf_compile_time_prng(seed, 1));
		static constexpr T PREMODMASK = (T(1) << (sizeof(T) * 4)) - 1;
		static constexpr T PREMOD = PREMODRNDCONST & PREMODMASK;
		static constexpr T MOD = PREMOD == 0 ? 100 : PREMOD;//remapping 'bad value' 0 to 'something'
		static constexpr T CC = obf_weak_random(obf_compile_time_prng(seed, 2),MOD);

		static constexpr T MAXMUL1 = T(-1)/MOD;
		static constexpr auto MAXMUL1_ADJUSTED0 = obf_sqrt_very_rough_approximation(MAXMUL1);
		static_assert(MAXMUL1_ADJUSTED0 < T(-1));
		static constexpr T MAXMUL1_ADJUSTED = (T)MAXMUL1_ADJUSTED0;
		static constexpr T MUL1 = MAXMUL1 > 2 ? 1+obf_weak_random(obf_compile_time_prng(seed, 2), MAXMUL1_ADJUSTED) : 1;
		static constexpr T DELTA = MUL1 * MOD;
		static_assert(DELTA / MUL1 == MOD);//overflow check

		static constexpr T MAXMUL2 = T(-1) / DELTA;
		static constexpr T MUL2 = MAXMUL2 > 2 ? 1+obf_weak_random(obf_compile_time_prng(seed, 3), MAXMUL2) : 1;
		static constexpr T DELTAMOD = MUL2 * MOD;
		static_assert(DELTAMOD / MUL2 == MOD);//overflow check

		static constexpr T PREMUL3 = obf_weak_random(obf_compile_time_prng(seed, 3), MUL2);
		static constexpr T MUL3 = PREMUL3 > 0 ? PREMUL3 : 1;
		static constexpr T CC0 = ( CC + MUL3 * MOD ) % DELTAMOD;

		static_assert((CC0 + DELTA) % MOD == CC);
		static constexpr bool test_n_iterations(T x, int n) {
			assert(x%MOD == CC);
			if (n == 0)
				return true;
			T newC = (x + DELTA) % DELTAMOD;
			assert(newC%MOD == CC);
			return test_n_iterations(newC,n-1);
		}
		static_assert(test_n_iterations(CC0, OBF_COMPILE_TIME_TESTS));//test only

		static constexpr T final_injection(T x) {
			return x + CC;
		}
		static T final_surjection(T y) {
			//{MT-related:
			T newC = (c+DELTA)%DELTAMOD;
			c = newC;
			//}MT-related
			assert(c%MOD == CC);
			return y - (c%MOD);
		}

#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
		static void dbgPrint(size_t offset = 0) {
			std::cout << std::string(offset, ' ') << "ObfLiteralContext_version<4/*global volatile var-with-invariant*/," << obf_dbgPrintT<T>() << "," << seed << ">: CC=" << CC << std::endl;
		}
#endif
	private:
#ifdef OBF_STRICT_MT
		static std::atomic<T> c;
#else
		static volatile T c;
#endif
	};

#ifdef OBF_STRICT_MT
	template<class T, OBFSEED seed>
	std::atomic<T> ObfLiteralContext_version<4, T, seed>::c = CC0;
#else
	template<class T, OBFSEED seed>
	volatile T ObfLiteralContext_version<4, T, seed>::c = CC0;
#endif

	//ObfZeroContext
	template<class T>
	struct ObfZeroContext {
		//uses the same injection as for ObfLiteralContext_version<0,...>,
		//  but has different ObfRecursiveContext semantics
		static constexpr T final_injection(T x) {
			return x;
		}
		static constexpr T final_surjection(T y) {
			return y;
		}

#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
		static void dbgPrint(size_t offset = 0) {
			std::cout << std::string(offset, ' ') << "ObfZeroContext<" << obf_dbgPrintT<T>() << ">" << std::endl;
		}
#endif
	};
	template<class T, class T0, OBFSEED seed, OBFCYCLES cycles>
	struct ObfRecursiveContext<T, ObfZeroContext<T0>, seed, cycles> {
		using recursive_context_type = ObfZeroContext<T>;
		using side_context_type = ObfZeroContext<T>;
	};

	//ObfLiteralContext
	template<class T, OBFSEED seed, OBFCYCLES cycles>
	class ObfLiteralContext {
		static_assert(std::is_integral<T>::value);
		static_assert(std::is_unsigned<T>::value);
		constexpr static std::array<ObfDescriptor, 5> descr{
			obf_literal_context_version0_descr::descr,
			obf_literal_context_version1_descr::descr,
			obf_literal_context_version2_descr::descr,
			obf_literal_context_version3_descr::descr,
			obf_literal_context_version4_descr::descr,
		};
		constexpr static size_t which = obf_random_obf_from_list(obf_compile_time_prng(seed, 1), cycles, descr);
		using WhichType = ObfLiteralContext_version<which, T, seed>;

	public:
		static constexpr T final_injection(T x) {
			return WhichType::final_injection(x);
		}
		static /*non-constexpr*/ T final_surjection(T y) {
			return WhichType::final_surjection(y);
		}

	public:
#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
		static void dbgPrint(size_t offset = 0) {
			size_t dbgWhich = obf_random_obf_from_list(obf_compile_time_prng(seed, 1), cycles, descr);
			std::cout << std::string(offset, ' ') << "ObfLiteralContext<" << obf_dbgPrintT<T>() << "," << seed << "," << cycles << ">: which=" << which << " dbgWhich=" << dbgWhich << std::endl;
			WhichType::dbgPrint(offset + 1);
		}
#endif
	};

	template<size_t which,class T, OBFSEED seed, OBFCYCLES cycles>
	struct obf_literal_side_context_version;
	template<class T, OBFSEED seed, OBFCYCLES cycles>
	struct obf_literal_side_context_version<0, T, seed, cycles> {
		using type = ObfZeroContext<T>;
	};
	template<class T, OBFSEED seed, OBFCYCLES cycles>
	struct obf_literal_side_context_version<1, T, seed, cycles> {
		using type = ObfLiteralContext<T, seed, cycles>;
	};

	template<class T, class T0, OBFSEED seed, OBFSEED seed0, OBFCYCLES cycles0,OBFCYCLES cycles>
	struct ObfRecursiveContext<T, ObfLiteralContext<T0, seed0,cycles0>, seed, cycles> {
		using recursive_context_type = ObfLiteralContext<T, seed,cycles>;

		constexpr static std::array<size_t, 2> weights{100,10};//TODO: #define
		static constexpr size_t which = obf_random_from_list(obf_compile_time_prng(seed,1), weights);
		using side_context_type = typename obf_literal_side_context_version<which,T, obf_compile_time_prng(seed, 2), cycles>::type;
	};

	//obf_literal
	template<class T_, T_ C_, OBFSEED seed, OBFCYCLES cycles>
	class obf_literal {
		static_assert(std::is_integral<T_>::value);
		using T = typename std::make_unsigned<T_>::type;//from this point on, unsigned only
		static constexpr T C = (T)C_;

		using Context = ObfLiteralContext<T, obf_compile_time_prng(seed, 1),cycles>;
		using Injection = obf_injection<T, Context, obf_compile_time_prng(seed, 2), cycles>;
	public:
		constexpr obf_literal() : val(Injection::injection(C)) {
		}
		T value() const {
			return Injection::surjection(val);
		}

#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
		static void dbgPrint(size_t offset = 0) {
			std::cout << std::string(offset, ' ') << "obf_literal<"<<obf_dbgPrintT<T>()<<"," << C << "," << seed << "," << cycles << ">" << std::endl;
			Injection::dbgPrint(offset + 1);
		}
#endif
	private:
		typename Injection::return_type val;
	};

	//ObfVarContext
	template<class T,OBFSEED seed,OBFCYCLES cycles>
	struct ObfVarContext {
		static constexpr T final_injection(T x) {
			return x;
		}
		static constexpr T final_surjection(T y) {
			return y;
		}

#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
		static void dbgPrint(size_t offset = 0) {
			std::cout << std::string(offset, ' ') << "ObfVarContext<" << obf_dbgPrintT<T>() << ">" << std::endl;
		}
#endif
	};
	template<class T, class T0, OBFSEED seed0, OBFCYCLES cycles0, OBFSEED seed, OBFCYCLES cycles>
	struct ObfRecursiveContext<T, ObfVarContext<T0,seed0,cycles0>, seed, cycles> {
		using recursive_context_type = ObfVarContext<T,seed,cycles>;
		using side_context_type = ObfVarContext<T,seed,cycles>;
	};

	//obf_var
	template<class T_, OBFSEED seed, OBFCYCLES cycles>
	class obf_var {
		static_assert(std::is_integral<T_>::value);
		using T = typename std::make_unsigned<T_>::type;//from this point on, unsigned only

		using Context = ObfVarContext<T, obf_compile_time_prng(seed, 1), cycles>;
		using Injection = obf_injection<T, Context, obf_compile_time_prng(seed, 2), cycles>;

	public:
		obf_var(T_ t) : val(Injection::injection(T(t))) {
		}
		template<class T2,OBFSEED seed2, OBFCYCLES cycles2>
		obf_var(obf_var<T2, seed2, cycles2> t) : val(Injection::injection(T(T_(t.value())))) {//TODO: randomized injection implementation
		}//TODO: template<obf_literal>
		obf_var& operator =(T_ t) {
			val = Injection::injection(T(t));//TODO: randomized injection implementation
			return *this;
		}
		template<class T2,OBFSEED seed2, OBFCYCLES cycles2>
		obf_var& operator =(obf_var<T2, seed2, cycles2> t) {
			val = Injection::injection(T(T_(t.value())));//TODO: randomized injection implementation
			return *this;
		}//TODO: template<obf_literal>
		T_ value() const {
			return T_(Injection::surjection(val));
		}

		operator T_() const { return value(); }
		obf_var& operator ++() { *this = value() + 1; return *this; }
		obf_var& operator --() { *this = value() - 1; return *this; }
		obf_var operator++(int) { obf_var ret = obf_var(value());  *this = value() + 1; return ret; }
		obf_var operator--(int) { obf_var ret = obf_var(value());  *this = value() + 1; return ret; }

		template<class T2>
		bool operator <(T2 t) { return value() < t; }
		template<class T2>
		bool operator >(T2 t) { return value() > t; }
		template<class T2>
		bool operator ==(T2 t) { return value() == t; }
		template<class T2>
		bool operator !=(T2 t) { return value() != t; }
		template<class T2>
		bool operator <=(T2 t) { return value() <= t; }
		template<class T2>
		bool operator >=(T2 t) { return value() >= t; }

		template<class T2, OBFSEED seed2, OBFCYCLES cycles2>
		bool operator <(obf_var<T2, seed2, cycles2> t) {
			return value() < t.value();
		}//TODO: template<obf_literal>(for ALL comparisons)
		template<class T2, OBFSEED seed2, OBFCYCLES cycles2>
		bool operator >(obf_var<T2, seed2, cycles2> t) {
			return value() > t.value();
		}
		template<class T2, OBFSEED seed2, OBFCYCLES cycles2>
		bool operator ==(obf_var<T2, seed2, cycles2> t) {
			return value() == t.value();
		}
		template<class T2, OBFSEED seed2, OBFCYCLES cycles2>
		bool operator !=(obf_var<T2, seed2, cycles2> t) {
			return value() != t.value();
		}
		template<class T2, OBFSEED seed2, OBFCYCLES cycles2>
		bool operator <=(obf_var<T2, seed2, cycles2> t) {
			return value() <= t.value();
		}
		template<class T2, OBFSEED seed2, OBFCYCLES cycles2>
		bool operator >=(obf_var<T2, seed2, cycles2> t) {
			return value() >= t.value();
		}

		template<class T2>
		obf_var& operator +=(T2 t) { *this = value() + t; return *this; }
		template<class T2>
		obf_var& operator -=(T2 t) { *this = value() - t; return *this; }
		template<class T2>
		obf_var& operator *=(T2 t) { *this = value() * t; return *this; }
		template<class T2>
		obf_var& operator /=(T2 t) { *this = value() / t; return *this; }
		template<class T2>
		obf_var& operator %=(T2 t) { *this = value() % t; return *this; }

		template<class T2, OBFSEED seed2, OBFCYCLES cycles2>
		obf_var& operator +=(obf_var<T2, seed2, cycles2> t) {
			return *this += t.value();
		}//TODO: template<obf_literal>(for ALL ?= operations)
		template<class T2, OBFSEED seed2, OBFCYCLES cycles2>
		obf_var& operator -=(obf_var<T2, seed2, cycles2> t) {
			return *this -= t.value();
		}
		template<class T2, OBFSEED seed2, OBFCYCLES cycles2>
		obf_var& operator *=(obf_var<T2, seed2, cycles2> t) {
			return *this *= t.value();
		}
		template<class T2, OBFSEED seed2, OBFCYCLES cycles2>
		obf_var& operator /=(obf_var<T2, seed2, cycles2> t) {
			return *this /= t.value();
		}
		template<class T2, OBFSEED seed2, OBFCYCLES cycles2>
		obf_var& operator %=(obf_var<T2, seed2, cycles2> t) {
			return *this %= t.value();
		}

		template<class T2>
		obf_var operator +(T2 t) { return obf_var(value()+t); }
		template<class T2>
		obf_var operator -(T2 t) { return obf_var(value() - t); }
		template<class T2>
		obf_var operator *(T2 t) { return obf_var(value() * t); }
		template<class T2>
		obf_var operator /(T2 t) { return obf_var(value() / t); }
		template<class T2>
		obf_var operator %(T2 t) { return obf_var(value() % t); }
		
		template<class T2, OBFSEED seed2, OBFCYCLES cycles2>//TODO: template<obf_literal_dbg>(for ALL binary operations)
		obf_var operator +(obf_var<T2,seed2,cycles2> t) { return obf_var(value() + t.value()); }
		template<class T2, OBFSEED seed2, OBFCYCLES cycles2>
		obf_var operator -(obf_var<T2, seed2, cycles2> t) { return obf_var(value() - t.value()); }
		template<class T2, OBFSEED seed2, OBFCYCLES cycles2>
		obf_var operator *(obf_var<T2, seed2, cycles2> t) { return obf_var(value() * t.value()); }
		template<class T2, OBFSEED seed2, OBFCYCLES cycles2>
		obf_var operator /(obf_var<T2, seed2, cycles2> t) { return obf_var(value() / t.value()); }
		template<class T2, OBFSEED seed2, OBFCYCLES cycles2>
		obf_var operator %(obf_var<T2, seed2, cycles2> t) { return obf_var(value() % t.value()); }

		//TODO: bitwise

#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
		static void dbgPrint(size_t offset = 0) {
			std::cout << std::string(offset, ' ') << "obf_var<" << obf_dbgPrintT<T>() << "," << seed <<","<<cycles<<">" << std::endl;
			Injection::dbgPrint(offset+1);
		}
#endif

	private:
		typename Injection::return_type val;
	};

	//external functions
	extern int obf_preMain();
	inline void obf_init() {
		obf_preMain();
	}

	//USER-LEVEL:
	/*think about it further //  obfN<> templates
	template<class T,OBFSEED seed>
	class obf0 {
		using Base = obf_var<T, seed, obf_exp_cycles(0)>;

	public:
		obf0() : val() {}
		obf0(T x) : val(x) {}
		obf0 operator =(T x) { val = x; return *this; }
		operator T() const { return val.value(); }

	private:
		Base val;
	};*/

}//namespace obf

 //macros; DON'T belong to the namespace...
#ifdef _MSC_VER
 //direct use of __LINE__ doesn't count as constexpr in MSVC - don't ask why...

 //along the lines of https://stackoverflow.com/questions/19343205/c-concatenating-file-and-line-macros:
#define OBF_S1(x) #x
#define OBF_S2(x) OBF_S1(x)
#define OBF_LOCATION __FILE__ " : " OBF_S2(__LINE__)

#define OBF0(type) obf::obf_var<type,obf::obf_seed_from_file_line(OBF_LOCATION,0),obf::obf_exp_cycles((OBFSCALE)+0)>
#define OBF1(type) obf::obf_var<type,obf::obf_seed_from_file_line(OBF_LOCATION,0),obf::obf_exp_cycles((OBFSCALE)+1)>
#define OBF2(type) obf::obf_var<type,obf::obf_seed_from_file_line(OBF_LOCATION,0),obf::obf_exp_cycles((OBFSCALE)+2)>
#define OBF3(type) obf::obf_var<type,obf::obf_seed_from_file_line(OBF_LOCATION,0),obf::obf_exp_cycles((OBFSCALE)+3)>
#define OBF4(type) obf::obf_var<type,obf::obf_seed_from_file_line(OBF_LOCATION,0),obf::obf_exp_cycles((OBFSCALE)+4)>
#define OBF5(type) obf::obf_var<type,obf::obf_seed_from_file_line(OBF_LOCATION,0),obf::obf_exp_cycles((OBFSCALE)+5)>
#else//_MSC_VER
#define OBF0(type) obf::obf_var<type,obf::obf_seed_from_file_line(__FILE__,__LINE__),obf::obf_exp_cycles((OBFSCALE)+0)>
#define OBF1(type) obf::obf_var<type,obf::obf_seed_from_file_line(__FILE__,__LINE__),obf::obf_exp_cycles((OBFSCALE)+1)>
#define OBF2(type) obf::obf_var<type,obf::obf_seed_from_file_line(__FILE__,__LINE__),obf::obf_exp_cycles((OBFSCALE)+2)>
#define OBF3(type) obf::obf_var<type,obf::obf_seed_from_file_line(__FILE__,__LINE__),obf::obf_exp_cycles((OBFSCALE)+3)>
#define OBF4(type) obf::obf_var<type,obf::obf_seed_from_file_line(__FILE__,__LINE__),obf::obf_exp_cycles((OBFSCALE)+4)>
#define OBF5(type) obf::obf_var<type,obf::obf_seed_from_file_line(__FILE__,__LINE__),obf::obf_exp_cycles((OBFSCALE)+5)>
#endif

#else//OBFUSCATE_SEED

	namespace obf {
#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
		//dbgPrint helpers
		template<class T>
		std::string obf_dbgPrintT() {
			return std::string("T(sizeof=") + std::to_string(sizeof(T)) + ")";
		}
#endif

		//obf_var_dbg
		template<class T>
		class obf_var_dbg {
			static_assert(std::is_integral<T>::value);

		public:
			obf_var_dbg(T t) : val(t) {
			}
			template<class T, class T2>
			obf_var_dbg(obf_var_dbg<T2> t) : val(T(t.value())) {
			}
			obf_var_dbg& operator =(T t) {
				val = t;
				return *this;
			}
			template<class T, class T2>
			obf_var_dbg& operator =(obf_var_dbg<T2> t) {
				val = T(t.value());
				return *this;
			}
			T value() const {
				return val;
			}

			operator T() const { return value(); }
			obf_var_dbg& operator ++() { *this = value() + 1; return *this; }
			obf_var_dbg& operator --() { *this = value() - 1; return *this; }
			obf_var_dbg operator++(int) { obf_var_dbg ret = obf_var_dbg(value());  *this = value() + 1; return ret; }
			obf_var_dbg operator--(int) { obf_var_dbg ret = obf_var_dbg(value());  *this = value() + 1; return ret; }

			template<class T2>
			bool operator <(T2 t) { return value() < t; }
			template<class T2>
			bool operator >(T2 t) { return value() > t; }
			template<class T2>
			bool operator ==(T2 t) { return value() == t; }
			template<class T2>
			bool operator !=(T2 t) { return value() != t; }
			template<class T2>
			bool operator <=(T2 t) { return value() <= t; }
			template<class T2>
			bool operator >=(T2 t) { return value() >= t; }

			template<class T2>
			bool operator <(obf_var_dbg<T2> t) {
				return value() < t.value();
			}//TODO: template<obf_literal_dbg>(for ALL comparisons)
			template<class T2>
			bool operator >(obf_var_dbg<T2> t) {
				return value() > t.value();
			}
			template<class T2>
			bool operator ==(obf_var_dbg<T2> t) {
				return value() == t.value();
			}
			template<class T2>
			bool operator !=(obf_var_dbg<T2> t) {
				return value() != t.value();
			}
			template<class T2>
			bool operator <=(obf_var_dbg<T2> t) {
				return value() <= t.value();
			}
			template<class T2>
			bool operator >=(obf_var_dbg<T2> t) {
				return value() >= t.value();
			}

			template<class T2>
			obf_var_dbg& operator +=(T2 t) { *this = value() + t; return *this; }
			template<class T2>
			obf_var_dbg& operator -=(T2 t) { *this = value() - t; return *this; }
			template<class T2>
			obf_var_dbg& operator *=(T2 t) { *this = value() * t; return *this; }
			template<class T2>
			obf_var_dbg& operator /=(T2 t) { *this = value() / t; return *this; }
			template<class T2>
			obf_var_dbg& operator %=(T2 t) { *this = value() % t; return *this; }

			template<class T2>
			obf_var_dbg& operator +=(obf_var_dbg<T2> t) {
				return *this += t.value();
			}//TODO: template<obf_literal_dbg>(for ALL ?= operations)
			template<class T2>
			obf_var_dbg& operator -=(obf_var_dbg<T2> t) {
				return *this -= t.value();
			}
			template<class T2>
			obf_var_dbg& operator *=(obf_var_dbg<T2> t) {
				return *this *= t.value();
			}
			template<class T2>
			obf_var_dbg& operator /=(obf_var_dbg<T2> t) {
				return *this /= t.value();
			}
			template<class T2>
			obf_var_dbg& operator %=(obf_var_dbg<T2> t) {
				return *this %= t.value();
			}

			template<class T2>
			obf_var_dbg operator +(T2 t) { return obf_var_dbg(value() + t); }
			template<class T2>
			obf_var_dbg operator -(T2 t) { return obf_var_dbg(value() - t); }
			template<class T2>
			obf_var_dbg operator *(T2 t) { return obf_var_dbg(value() * t); }
			template<class T2>
			obf_var_dbg operator /(T2 t) { return obf_var_dbg(value() / t); }
			template<class T2>
			obf_var_dbg operator %(T2 t) { return obf_var_dbg(value() % t); }

			template<class T2>//TODO: template<obf_literal_dbg>(for ALL binary operations)
			obf_var_dbg operator +(obf_var_dbg<T2> t) { return obf_var_dbg(value() + t.value()); }
			template<class T2>
			obf_var_dbg operator -(obf_var_dbg<T2> t) { return obf_var_dbg(value() - t.value()); }
			template<class T2>
			obf_var_dbg operator *(obf_var_dbg<T2> t) { return obf_var_dbg(value() * t.value()); }
			template<class T2>
			obf_var_dbg operator /(obf_var_dbg<T2> t) { return obf_var_dbg(value() / t.value()); }
			template<class T2>
			obf_var_dbg operator %(obf_var_dbg<T2> t) { return obf_var_dbg(value() % t.value()); }

			//TODO: bitwise

#ifdef OBFUSCATE_DEBUG_ENABLE_DBGPRINT
			static void dbgPrint(size_t offset = 0) {
				std::cout << std::string(offset, ' ') << "obf_var_dbg<" << obf_dbgPrintT<T>() << ">" << std::endl;
			}
#endif

		private:
			typename T val;
		};

		inline void obf_init() {
		}

	}//namespace obf

#define OBF0(type) obf::obf_var_dbg<type>
#define OBF1(type) obf::obf_var_dbg<type>
#define OBF2(type) obf::obf_var_dbg<type>
#define OBF3(type) obf::obf_var_dbg<type>
#define OBF4(type) obf::obf_var_dbg<type>
#define OBF5(type) obf::obf_var_dbg<type>

#endif //OBFUSCATE_SEED

#endif//obfuscate_h_included