/**
 *******************************************************************************
 * Copyright (C) 2006-2014, International Business Machines Corporation
 * and others. All Rights Reserved.
 *******************************************************************************
 */

#include "unicode/utypes.h"

#if !UCONFIG_NO_BREAK_ITERATION

#include "brkeng.h"
#include "dictbe.h"
#include "unicode/uniset.h"
#include "unicode/chariter.h"
#include "unicode/ubrk.h"
#include "uvectr32.h"
#include "uvector.h"
#include "uassert.h"
#include "unicode/normlzr.h"
#include "cmemory.h"
#include "dictionarydata.h"

U_NAMESPACE_BEGIN

/*
 ******************************************************************
 */

DictionaryBreakEngine::DictionaryBreakEngine(uint32_t breakTypes) {
    fTypes = breakTypes;
}

DictionaryBreakEngine::~DictionaryBreakEngine() {
}

UBool
DictionaryBreakEngine::handles(UChar32 c, int32_t breakType) const {
    return (breakType >= 0 && breakType < 32 && (((uint32_t)1 << breakType) & fTypes)
            && fSet.contains(c));
}

int32_t
DictionaryBreakEngine::findBreaks( UText *text,
                                 int32_t startPos,
                                 int32_t endPos,
                                 UBool reverse,
                                 int32_t breakType,
                                 UStack &foundBreaks ) const {
    int32_t result = 0;

    // Find the span of characters included in the set.
    //   The span to break begins at the current position in the text, and
    //   extends towards the start or end of the text, depending on 'reverse'.

    int32_t start = (int32_t)utext_getNativeIndex(text);
    int32_t current;
    int32_t rangeStart;
    int32_t rangeEnd;
    UChar32 c = utext_current32(text);
    if (reverse) {
        UBool   isDict = fSet.contains(c);
        while((current = (int32_t)utext_getNativeIndex(text)) > startPos && isDict) {
            c = utext_previous32(text);
            isDict = fSet.contains(c);
        }
        //rangeStart = (current < startPos) ? startPos : current+(isDict ? 0 : 1);
        if (current < startPos) {
            rangeStart = startPos;
        } else {
            rangeStart = current;
            if (!isDict) {
                utext_next32(text);
                rangeStart = utext_getNativeIndex(text);
            }
        }
        // rangeEnd = start + 1;
        utext_setNativeIndex(text, start);
        utext_next32(text);
        rangeEnd = utext_getNativeIndex(text);
    }
    else {
        while((current = (int32_t)utext_getNativeIndex(text)) < endPos && fSet.contains(c)) {
            utext_next32(text);         // TODO:  recast loop for postincrement
            c = utext_current32(text);
        }
        rangeStart = start;
        rangeEnd = current;
    }
    if (breakType >= 0 && breakType < 32 && (((uint32_t)1 << breakType) & fTypes)) {
        result = divideUpDictionaryRange(text, rangeStart, rangeEnd, foundBreaks);
        utext_setNativeIndex(text, current);
    }
    
    return result;
}

void
DictionaryBreakEngine::setCharacters( const UnicodeSet &set ) {
    fSet = set;
    // Compact for caching
    fSet.compact();
}

/*
 ******************************************************************
 * PossibleWord
 */

// Helper class for improving readability of the Thai/Lao/Khmer word break
// algorithm. The implementation is completely inline.

// List size, limited by the maximum number of words in the dictionary
// that form a nested sequence.
#define POSSIBLE_WORD_LIST_MAX 20

class PossibleWord {
private:
    // list of word candidate lengths, in increasing length order
    int32_t   lengths[POSSIBLE_WORD_LIST_MAX];
    int32_t   count;      // Count of candidates
    int32_t   prefix;     // The longest match with a dictionary word
    int32_t   offset;     // Offset in the text of these candidates
    int       mark;       // The preferred candidate's offset
    int       current;    // The candidate we're currently looking at

public:
    PossibleWord();
    ~PossibleWord();
  
    // Fill the list of candidates if needed, select the longest, and return the number found
    int       candidates( UText *text, DictionaryMatcher *dict, int32_t rangeEnd );
  
    // Select the currently marked candidate, point after it in the text, and invalidate self
    int32_t   acceptMarked( UText *text );
  
    // Back up from the current candidate to the next shorter one; return TRUE if that exists
    // and point the text after it
    UBool     backUp( UText *text );
  
    // Return the longest prefix this candidate location shares with a dictionary word
    int32_t   longestPrefix();
  
    // Mark the current candidate as the one we like
    void      markCurrent();
};

inline
PossibleWord::PossibleWord() {
    offset = -1;
}

inline
PossibleWord::~PossibleWord() {
}

inline int
PossibleWord::candidates( UText *text, DictionaryMatcher *dict, int32_t rangeEnd ) {
    // TODO: If getIndex is too slow, use offset < 0 and add discardAll()
    int32_t start = (int32_t)utext_getNativeIndex(text);
    if (start != offset) {
        offset = start;
        prefix = dict->matches(text, rangeEnd-start, lengths, count, sizeof(lengths)/sizeof(lengths[0]));
        // Dictionary leaves text after longest prefix, not longest word. Back up.
        if (count <= 0) {
            utext_setNativeIndex(text, start);
        }
    }
    if (count > 0) {
        utext_setNativeIndex(text, start+lengths[count-1]);
    }
    current = count-1;
    mark = current;
    return count;
}

inline int32_t
PossibleWord::acceptMarked( UText *text ) {
    utext_setNativeIndex(text, offset + lengths[mark]);
    return lengths[mark];
}

inline UBool
PossibleWord::backUp( UText *text ) {
    if (current > 0) {
        utext_setNativeIndex(text, offset + lengths[--current]);
        return TRUE;
    }
    return FALSE;
}

inline int32_t
PossibleWord::longestPrefix() {
    return prefix;
}

inline void
PossibleWord::markCurrent() {
    mark = current;
}

/*
 ******************************************************************
 * ThaiBreakEngine
 */

// How many words in a row are "good enough"?
#define THAI_LOOKAHEAD 3

// Will not combine a non-word with a preceding dictionary word longer than this
#define THAI_ROOT_COMBINE_THRESHOLD 3

// Will not combine a non-word that shares at least this much prefix with a
// dictionary word, with a preceding word
#define THAI_PREFIX_COMBINE_THRESHOLD 3

// Ellision character
#define THAI_PAIYANNOI 0x0E2F

// Repeat character
#define THAI_MAIYAMOK 0x0E46

// Minimum word size
#define THAI_MIN_WORD 2

// Minimum number of characters for two words
#define THAI_MIN_WORD_SPAN (THAI_MIN_WORD * 2)

ThaiBreakEngine::ThaiBreakEngine(DictionaryMatcher *adoptDictionary, UErrorCode &status)
    : DictionaryBreakEngine((1<<UBRK_WORD) | (1<<UBRK_LINE)),
      fDictionary(adoptDictionary)
{
    fThaiWordSet.applyPattern(UNICODE_STRING_SIMPLE("[[:Thai:]&[:LineBreak=SA:]]"), status);
    if (U_SUCCESS(status)) {
        setCharacters(fThaiWordSet);
    }
    fMarkSet.applyPattern(UNICODE_STRING_SIMPLE("[[:Thai:]&[:LineBreak=SA:]&[:M:]]"), status);
    fMarkSet.add(0x0020);
    fEndWordSet = fThaiWordSet;
    fEndWordSet.remove(0x0E31);             // MAI HAN-AKAT
    fEndWordSet.remove(0x0E40, 0x0E44);     // SARA E through SARA AI MAIMALAI
    fBeginWordSet.add(0x0E01, 0x0E2E);      // KO KAI through HO NOKHUK
    fBeginWordSet.add(0x0E40, 0x0E44);      // SARA E through SARA AI MAIMALAI
    fSuffixSet.add(THAI_PAIYANNOI);
    fSuffixSet.add(THAI_MAIYAMOK);

    // Compact for caching.
    fMarkSet.compact();
    fEndWordSet.compact();
    fBeginWordSet.compact();
    fSuffixSet.compact();
}

ThaiBreakEngine::~ThaiBreakEngine() {
    delete fDictionary;
}

int32_t
ThaiBreakEngine::divideUpDictionaryRange( UText *text,
                                                int32_t rangeStart,
                                                int32_t rangeEnd,
                                                UStack &foundBreaks ) const {
    if ((rangeEnd - rangeStart) < THAI_MIN_WORD_SPAN) {
        return 0;       // Not enough characters for two words
    }

    uint32_t wordsFound = 0;
    int32_t wordLength;
    int32_t current;
    UErrorCode status = U_ZERO_ERROR;
    PossibleWord words[THAI_LOOKAHEAD];
    UChar32 uc;
    
    utext_setNativeIndex(text, rangeStart);
    
    while (U_SUCCESS(status) && (current = (int32_t)utext_getNativeIndex(text)) < rangeEnd) {
        wordLength = 0;

        // Look for candidate words at the current position
        int candidates = words[wordsFound%THAI_LOOKAHEAD].candidates(text, fDictionary, rangeEnd);
        
        // If we found exactly one, use that
        if (candidates == 1) {
            wordLength = words[wordsFound % THAI_LOOKAHEAD].acceptMarked(text);
            wordsFound += 1;
        }
        // If there was more than one, see which one can take us forward the most words
        else if (candidates > 1) {
            // If we're already at the end of the range, we're done
            if ((int32_t)utext_getNativeIndex(text) >= rangeEnd) {
                goto foundBest;
            }
            do {
                int wordsMatched = 1;
                if (words[(wordsFound + 1) % THAI_LOOKAHEAD].candidates(text, fDictionary, rangeEnd) > 0) {
                    if (wordsMatched < 2) {
                        // Followed by another dictionary word; mark first word as a good candidate
                        words[wordsFound%THAI_LOOKAHEAD].markCurrent();
                        wordsMatched = 2;
                    }
                    
                    // If we're already at the end of the range, we're done
                    if ((int32_t)utext_getNativeIndex(text) >= rangeEnd) {
                        goto foundBest;
                    }
                    
                    // See if any of the possible second words is followed by a third word
                    do {
                        // If we find a third word, stop right away
                        if (words[(wordsFound + 2) % THAI_LOOKAHEAD].candidates(text, fDictionary, rangeEnd)) {
                            words[wordsFound % THAI_LOOKAHEAD].markCurrent();
                            goto foundBest;
                        }
                    }
                    while (words[(wordsFound + 1) % THAI_LOOKAHEAD].backUp(text));
                }
            }
            while (words[wordsFound % THAI_LOOKAHEAD].backUp(text));
foundBest:
            wordLength = words[wordsFound % THAI_LOOKAHEAD].acceptMarked(text);
            wordsFound += 1;
        }
        
        // We come here after having either found a word or not. We look ahead to the
        // next word. If it's not a dictionary word, we will combine it withe the word we
        // just found (if there is one), but only if the preceding word does not exceed
        // the threshold.
        // The text iterator should now be positioned at the end of the word we found.
        if ((int32_t)utext_getNativeIndex(text) < rangeEnd && wordLength < THAI_ROOT_COMBINE_THRESHOLD) {
            // if it is a dictionary word, do nothing. If it isn't, then if there is
            // no preceding word, or the non-word shares less than the minimum threshold
            // of characters with a dictionary word, then scan to resynchronize
            if (words[wordsFound % THAI_LOOKAHEAD].candidates(text, fDictionary, rangeEnd) <= 0
                  && (wordLength == 0
                      || words[wordsFound%THAI_LOOKAHEAD].longestPrefix() < THAI_PREFIX_COMBINE_THRESHOLD)) {
                // Look for a plausible word boundary
                //TODO: This section will need a rework for UText.
                int32_t remaining = rangeEnd - (current+wordLength);
                UChar32 pc = utext_current32(text);
                int32_t chars = 0;
                for (;;) {
                    utext_next32(text);
                    uc = utext_current32(text);
                    // TODO: Here we're counting on the fact that the SA languages are all
                    // in the BMP. This should get fixed with the UText rework.
                    chars += 1;
                    if (--remaining <= 0) {
                        break;
                    }
                    if (fEndWordSet.contains(pc) && fBeginWordSet.contains(uc)) {
                        // Maybe. See if it's in the dictionary.
                        // NOTE: In the original Apple code, checked that the next
                        // two characters after uc were not 0x0E4C THANTHAKHAT before
                        // checking the dictionary. That is just a performance filter,
                        // but it's not clear it's faster than checking the trie.
                        int candidates = words[(wordsFound + 1) % THAI_LOOKAHEAD].candidates(text, fDictionary, rangeEnd);
                        utext_setNativeIndex(text, current + wordLength + chars);
                        if (candidates > 0) {
                            break;
                        }
                    }
                    pc = uc;
                }
                
                // Bump the word count if there wasn't already one
                if (wordLength <= 0) {
                    wordsFound += 1;
                }
                
                // Update the length with the passed-over characters
                wordLength += chars;
            }
            else {
                // Back up to where we were for next iteration
                utext_setNativeIndex(text, current+wordLength);
            }
        }
        
        // Never stop before a combining mark.
        int32_t currPos;
        while ((currPos = (int32_t)utext_getNativeIndex(text)) < rangeEnd && fMarkSet.contains(utext_current32(text))) {
            utext_next32(text);
            wordLength += (int32_t)utext_getNativeIndex(text) - currPos;
        }
        
        // Look ahead for possible suffixes if a dictionary word does not follow.
        // We do this in code rather than using a rule so that the heuristic
        // resynch continues to function. For example, one of the suffix characters
        // could be a typo in the middle of a word.
        if ((int32_t)utext_getNativeIndex(text) < rangeEnd && wordLength > 0) {
            if (words[wordsFound%THAI_LOOKAHEAD].candidates(text, fDictionary, rangeEnd) <= 0
                && fSuffixSet.contains(uc = utext_current32(text))) {
                if (uc == THAI_PAIYANNOI) {
                    if (!fSuffixSet.contains(utext_previous32(text))) {
                        // Skip over previous end and PAIYANNOI
                        utext_next32(text);
                        utext_next32(text);
                        wordLength += 1;            // Add PAIYANNOI to word
                        uc = utext_current32(text);     // Fetch next character
                    }
                    else {
                        // Restore prior position
                        utext_next32(text);
                    }
                }
                if (uc == THAI_MAIYAMOK) {
                    if (utext_previous32(text) != THAI_MAIYAMOK) {
                        // Skip over previous end and MAIYAMOK
                        utext_next32(text);
                        utext_next32(text);
                        wordLength += 1;            // Add MAIYAMOK to word
                    }
                    else {
                        // Restore prior position
                        utext_next32(text);
                    }
                }
            }
            else {
                utext_setNativeIndex(text, current+wordLength);
            }
        }

        // Did we find a word on this iteration? If so, push it on the break stack
        if (wordLength > 0) {
            foundBreaks.push((current+wordLength), status);
        }
    }

    // Don't return a break for the end of the dictionary range if there is one there.
    if (foundBreaks.peeki() >= rangeEnd) {
        (void) foundBreaks.popi();
        wordsFound -= 1;
    }

    return wordsFound;
}

/*
 ******************************************************************
 * LaoBreakEngine
 */

// How many words in a row are "good enough"?
#define LAO_LOOKAHEAD 3

// Will not combine a non-word with a preceding dictionary word longer than this
#define LAO_ROOT_COMBINE_THRESHOLD 3

// Will not combine a non-word that shares at least this much prefix with a
// dictionary word, with a preceding word
#define LAO_PREFIX_COMBINE_THRESHOLD 3

// Minimum word size
#define LAO_MIN_WORD 2

// Minimum number of characters for two words
#define LAO_MIN_WORD_SPAN (LAO_MIN_WORD * 2)

LaoBreakEngine::LaoBreakEngine(DictionaryMatcher *adoptDictionary, UErrorCode &status)
    : DictionaryBreakEngine((1<<UBRK_WORD) | (1<<UBRK_LINE)),
      fDictionary(adoptDictionary)
{
    fLaoWordSet.applyPattern(UNICODE_STRING_SIMPLE("[[:Laoo:]&[:LineBreak=SA:]]"), status);
    if (U_SUCCESS(status)) {
        setCharacters(fLaoWordSet);
    }
    fMarkSet.applyPattern(UNICODE_STRING_SIMPLE("[[:Laoo:]&[:LineBreak=SA:]&[:M:]]"), status);
    fMarkSet.add(0x0020);
    fEndWordSet = fLaoWordSet;
    fEndWordSet.remove(0x0EC0, 0x0EC4);     // prefix vowels
    fBeginWordSet.add(0x0E81, 0x0EAE);      // basic consonants (including holes for corresponding Thai characters)
    fBeginWordSet.add(0x0EDC, 0x0EDD);      // digraph consonants (no Thai equivalent)
    fBeginWordSet.add(0x0EC0, 0x0EC4);      // prefix vowels

    // Compact for caching.
    fMarkSet.compact();
    fEndWordSet.compact();
    fBeginWordSet.compact();
}

LaoBreakEngine::~LaoBreakEngine() {
    delete fDictionary;
}

int32_t
LaoBreakEngine::divideUpDictionaryRange( UText *text,
                                                int32_t rangeStart,
                                                int32_t rangeEnd,
                                                UStack &foundBreaks ) const {
    if ((rangeEnd - rangeStart) < LAO_MIN_WORD_SPAN) {
        return 0;       // Not enough characters for two words
    }

    uint32_t wordsFound = 0;
    int32_t wordLength;
    int32_t current;
    UErrorCode status = U_ZERO_ERROR;
    PossibleWord words[LAO_LOOKAHEAD];
    UChar32 uc;
    
    utext_setNativeIndex(text, rangeStart);
    
    while (U_SUCCESS(status) && (current = (int32_t)utext_getNativeIndex(text)) < rangeEnd) {
        wordLength = 0;

        // Look for candidate words at the current position
        int candidates = words[wordsFound%LAO_LOOKAHEAD].candidates(text, fDictionary, rangeEnd);
        
        // If we found exactly one, use that
        if (candidates == 1) {
            wordLength = words[wordsFound % LAO_LOOKAHEAD].acceptMarked(text);
            wordsFound += 1;
        }
        // If there was more than one, see which one can take us forward the most words
        else if (candidates > 1) {
            // If we're already at the end of the range, we're done
            if ((int32_t)utext_getNativeIndex(text) >= rangeEnd) {
                goto foundBest;
            }
            do {
                int wordsMatched = 1;
                if (words[(wordsFound + 1) % LAO_LOOKAHEAD].candidates(text, fDictionary, rangeEnd) > 0) {
                    if (wordsMatched < 2) {
                        // Followed by another dictionary word; mark first word as a good candidate
                        words[wordsFound%LAO_LOOKAHEAD].markCurrent();
                        wordsMatched = 2;
                    }
                    
                    // If we're already at the end of the range, we're done
                    if ((int32_t)utext_getNativeIndex(text) >= rangeEnd) {
                        goto foundBest;
                    }
                    
                    // See if any of the possible second words is followed by a third word
                    do {
                        // If we find a third word, stop right away
                        if (words[(wordsFound + 2) % LAO_LOOKAHEAD].candidates(text, fDictionary, rangeEnd)) {
                            words[wordsFound % LAO_LOOKAHEAD].markCurrent();
                            goto foundBest;
                        }
                    }
                    while (words[(wordsFound + 1) % LAO_LOOKAHEAD].backUp(text));
                }
            }
            while (words[wordsFound % LAO_LOOKAHEAD].backUp(text));
foundBest:
            wordLength = words[wordsFound % LAO_LOOKAHEAD].acceptMarked(text);
            wordsFound += 1;
        }
        
        // We come here after having either found a word or not. We look ahead to the
        // next word. If it's not a dictionary word, we will combine it withe the word we
        // just found (if there is one), but only if the preceding word does not exceed
        // the threshold.
        // The text iterator should now be positioned at the end of the word we found.
        if ((int32_t)utext_getNativeIndex(text) < rangeEnd && wordLength < LAO_ROOT_COMBINE_THRESHOLD) {
            // if it is a dictionary word, do nothing. If it isn't, then if there is
            // no preceding word, or the non-word shares less than the minimum threshold
            // of characters with a dictionary word, then scan to resynchronize
            if (words[wordsFound % LAO_LOOKAHEAD].candidates(text, fDictionary, rangeEnd) <= 0
                  && (wordLength == 0
                      || words[wordsFound%LAO_LOOKAHEAD].longestPrefix() < LAO_PREFIX_COMBINE_THRESHOLD)) {
                // Look for a plausible word boundary
                //TODO: This section will need a rework for UText.
                int32_t remaining = rangeEnd - (current+wordLength);
                UChar32 pc = utext_current32(text);
                int32_t chars = 0;
                for (;;) {
                    utext_next32(text);
                    uc = utext_current32(text);
                    // TODO: Here we're counting on the fact that the SA languages are all
                    // in the BMP. This should get fixed with the UText rework.
                    chars += 1;
                    if (--remaining <= 0) {
                        break;
                    }
                    if (fEndWordSet.contains(pc) && fBeginWordSet.contains(uc)) {
                        // Maybe. See if it's in the dictionary.
                        int candidates = words[(wordsFound + 1) % LAO_LOOKAHEAD].candidates(text, fDictionary, rangeEnd);
                        utext_setNativeIndex(text, current + wordLength + chars);
                        if (candidates > 0) {
                            break;
                        }
                    }
                    pc = uc;
                }
                
                // Bump the word count if there wasn't already one
                if (wordLength <= 0) {
                    wordsFound += 1;
                }
                
                // Update the length with the passed-over characters
                wordLength += chars;
            }
            else {
                // Back up to where we were for next iteration
                utext_setNativeIndex(text, current+wordLength);
            }
        }
        
        // Never stop before a combining mark.
        int32_t currPos;
        while ((currPos = (int32_t)utext_getNativeIndex(text)) < rangeEnd && fMarkSet.contains(utext_current32(text))) {
            utext_next32(text);
            wordLength += (int32_t)utext_getNativeIndex(text) - currPos;
        }
        
        // Look ahead for possible suffixes if a dictionary word does not follow.
        // We do this in code rather than using a rule so that the heuristic
        // resynch continues to function. For example, one of the suffix characters
        // could be a typo in the middle of a word.
        // NOT CURRENTLY APPLICABLE TO LAO

        // Did we find a word on this iteration? If so, push it on the break stack
        if (wordLength > 0) {
            foundBreaks.push((current+wordLength), status);
        }
    }

    // Don't return a break for the end of the dictionary range if there is one there.
    if (foundBreaks.peeki() >= rangeEnd) {
        (void) foundBreaks.popi();
        wordsFound -= 1;
    }

    return wordsFound;
}

/*
 ******************************************************************
 * KhmerBreakEngine
 */

// How many words in a row are "good enough"?
#define KHMER_LOOKAHEAD 3

// Will not combine a non-word with a preceding dictionary word longer than this
#define KHMER_ROOT_COMBINE_THRESHOLD 3

// Will not combine a non-word that shares at least this much prefix with a
// dictionary word, with a preceding word
#define KHMER_PREFIX_COMBINE_THRESHOLD 3

// Minimum word size
#define KHMER_MIN_WORD 2

// Minimum number of characters for two words
#define KHMER_MIN_WORD_SPAN (KHMER_MIN_WORD * 2)

KhmerBreakEngine::KhmerBreakEngine(DictionaryMatcher *adoptDictionary, UErrorCode &status)
    : DictionaryBreakEngine((1 << UBRK_WORD) | (1 << UBRK_LINE)),
      fDictionary(adoptDictionary)
{
    fKhmerWordSet.applyPattern(UNICODE_STRING_SIMPLE("[[:Khmr:]&[:LineBreak=SA:]]"), status);
    if (U_SUCCESS(status)) {
        setCharacters(fKhmerWordSet);
    }
    fMarkSet.applyPattern(UNICODE_STRING_SIMPLE("[[:Khmr:]&[:LineBreak=SA:]&[:M:]]"), status);
    fMarkSet.add(0x0020);
    fEndWordSet = fKhmerWordSet;
    fBeginWordSet.add(0x1780, 0x17B3);
    //fBeginWordSet.add(0x17A3, 0x17A4);      // deprecated vowels
    //fEndWordSet.remove(0x17A5, 0x17A9);     // Khmer independent vowels that can't end a word
    //fEndWordSet.remove(0x17B2);             // Khmer independent vowel that can't end a word
    fEndWordSet.remove(0x17D2);             // KHMER SIGN COENG that combines some following characters
    //fEndWordSet.remove(0x17B6, 0x17C5);     // Remove dependent vowels
//    fEndWordSet.remove(0x0E31);             // MAI HAN-AKAT
//    fEndWordSet.remove(0x0E40, 0x0E44);     // SARA E through SARA AI MAIMALAI
//    fBeginWordSet.add(0x0E01, 0x0E2E);      // KO KAI through HO NOKHUK
//    fBeginWordSet.add(0x0E40, 0x0E44);      // SARA E through SARA AI MAIMALAI
//    fSuffixSet.add(THAI_PAIYANNOI);
//    fSuffixSet.add(THAI_MAIYAMOK);

    // Compact for caching.
    fMarkSet.compact();
    fEndWordSet.compact();
    fBeginWordSet.compact();
//    fSuffixSet.compact();
}

KhmerBreakEngine::~KhmerBreakEngine() {
    delete fDictionary;
}

int32_t
KhmerBreakEngine::divideUpDictionaryRange( UText *text,
                                                int32_t rangeStart,
                                                int32_t rangeEnd,
                                                UStack &foundBreaks ) const {
    if ((rangeEnd - rangeStart) < KHMER_MIN_WORD_SPAN) {
        return 0;       // Not enough characters for two words
    }

    uint32_t wordsFound = 0;
    int32_t wordLength;
    int32_t current;
    UErrorCode status = U_ZERO_ERROR;
    PossibleWord words[KHMER_LOOKAHEAD];
    UChar32 uc;

    utext_setNativeIndex(text, rangeStart);

    while (U_SUCCESS(status) && (current = (int32_t)utext_getNativeIndex(text)) < rangeEnd) {
        wordLength = 0;

        // Look for candidate words at the current position
        int candidates = words[wordsFound%KHMER_LOOKAHEAD].candidates(text, fDictionary, rangeEnd);

        // If we found exactly one, use that
        if (candidates == 1) {
            wordLength = words[wordsFound%KHMER_LOOKAHEAD].acceptMarked(text);
            wordsFound += 1;
        }

        // If there was more than one, see which one can take us forward the most words
        else if (candidates > 1) {
            // If we're already at the end of the range, we're done
            if ((int32_t)utext_getNativeIndex(text) >= rangeEnd) {
                goto foundBest;
            }
            do {
                int wordsMatched = 1;
                if (words[(wordsFound + 1) % KHMER_LOOKAHEAD].candidates(text, fDictionary, rangeEnd) > 0) {
                    if (wordsMatched < 2) {
                        // Followed by another dictionary word; mark first word as a good candidate
                        words[wordsFound % KHMER_LOOKAHEAD].markCurrent();
                        wordsMatched = 2;
                    }

                    // If we're already at the end of the range, we're done
                    if ((int32_t)utext_getNativeIndex(text) >= rangeEnd) {
                        goto foundBest;
                    }

                    // See if any of the possible second words is followed by a third word
                    do {
                        // If we find a third word, stop right away
                        if (words[(wordsFound + 2) % KHMER_LOOKAHEAD].candidates(text, fDictionary, rangeEnd)) {
                            words[wordsFound % KHMER_LOOKAHEAD].markCurrent();
                            goto foundBest;
                        }
                    }
                    while (words[(wordsFound + 1) % KHMER_LOOKAHEAD].backUp(text));
                }
            }
            while (words[wordsFound % KHMER_LOOKAHEAD].backUp(text));
foundBest:
            wordLength = words[wordsFound % KHMER_LOOKAHEAD].acceptMarked(text);
            wordsFound += 1;
        }

        // We come here after having either found a word or not. We look ahead to the
        // next word. If it's not a dictionary word, we will combine it with the word we
        // just found (if there is one), but only if the preceding word does not exceed
        // the threshold.
        // The text iterator should now be positioned at the end of the word we found.
        if ((int32_t)utext_getNativeIndex(text) < rangeEnd && wordLength < KHMER_ROOT_COMBINE_THRESHOLD) {
            // if it is a dictionary word, do nothing. If it isn't, then if there is
            // no preceding word, or the non-word shares less than the minimum threshold
            // of characters with a dictionary word, then scan to resynchronize
            if (words[wordsFound % KHMER_LOOKAHEAD].candidates(text, fDictionary, rangeEnd) <= 0
                  && (wordLength == 0
                      || words[wordsFound % KHMER_LOOKAHEAD].longestPrefix() < KHMER_PREFIX_COMBINE_THRESHOLD)) {
                // Look for a plausible word boundary
                //TODO: This section will need a rework for UText.
                int32_t remaining = rangeEnd - (current+wordLength);
                UChar32 pc = utext_current32(text);
                int32_t chars = 0;
                for (;;) {
                    utext_next32(text);
                    uc = utext_current32(text);
                    // TODO: Here we're counting on the fact that the SA languages are all
                    // in the BMP. This should get fixed with the UText rework.
                    chars += 1;
                    if (--remaining <= 0) {
                        break;
                    }
                    if (fEndWordSet.contains(pc) && fBeginWordSet.contains(uc)) {
                        // Maybe. See if it's in the dictionary.
                        int candidates = words[(wordsFound + 1) % KHMER_LOOKAHEAD].candidates(text, fDictionary, rangeEnd);
                        utext_setNativeIndex(text, current+wordLength+chars);
                        if (candidates > 0) {
                            break;
                        }
                    }
                    pc = uc;
                }

                // Bump the word count if there wasn't already one
                if (wordLength <= 0) {
                    wordsFound += 1;
                }

                // Update the length with the passed-over characters
                wordLength += chars;
            }
            else {
                // Back up to where we were for next iteration
                utext_setNativeIndex(text, current+wordLength);
            }
        }

        // Never stop before a combining mark.
        int32_t currPos;
        while ((currPos = (int32_t)utext_getNativeIndex(text)) < rangeEnd && fMarkSet.contains(utext_current32(text))) {
            utext_next32(text);
            wordLength += (int32_t)utext_getNativeIndex(text) - currPos;
        }

        // Look ahead for possible suffixes if a dictionary word does not follow.
        // We do this in code rather than using a rule so that the heuristic
        // resynch continues to function. For example, one of the suffix characters
        // could be a typo in the middle of a word.
//        if ((int32_t)utext_getNativeIndex(text) < rangeEnd && wordLength > 0) {
//            if (words[wordsFound%KHMER_LOOKAHEAD].candidates(text, fDictionary, rangeEnd) <= 0
//                && fSuffixSet.contains(uc = utext_current32(text))) {
//                if (uc == KHMER_PAIYANNOI) {
//                    if (!fSuffixSet.contains(utext_previous32(text))) {
//                        // Skip over previous end and PAIYANNOI
//                        utext_next32(text);
//                        utext_next32(text);
//                        wordLength += 1;            // Add PAIYANNOI to word
//                        uc = utext_current32(text);     // Fetch next character
//                    }
//                    else {
//                        // Restore prior position
//                        utext_next32(text);
//                    }
//                }
//                if (uc == KHMER_MAIYAMOK) {
//                    if (utext_previous32(text) != KHMER_MAIYAMOK) {
//                        // Skip over previous end and MAIYAMOK
//                        utext_next32(text);
//                        utext_next32(text);
//                        wordLength += 1;            // Add MAIYAMOK to word
//                    }
//                    else {
//                        // Restore prior position
//                        utext_next32(text);
//                    }
//                }
//            }
//            else {
//                utext_setNativeIndex(text, current+wordLength);
//            }
//        }

        // Did we find a word on this iteration? If so, push it on the break stack
        if (wordLength > 0) {
            foundBreaks.push((current+wordLength), status);
        }
    }
    
    // Don't return a break for the end of the dictionary range if there is one there.
    if (foundBreaks.peeki() >= rangeEnd) {
        (void) foundBreaks.popi();
        wordsFound -= 1;
    }

    return wordsFound;
}

#if !UCONFIG_NO_NORMALIZATION
/*
 ******************************************************************
 * CjkBreakEngine
 */
static const uint32_t kuint32max = 0xFFFFFFFF;
CjkBreakEngine::CjkBreakEngine(DictionaryMatcher *adoptDictionary, LanguageType type, UErrorCode &status)
: DictionaryBreakEngine(1 << UBRK_WORD), fDictionary(adoptDictionary) {
    // Korean dictionary only includes Hangul syllables
    fHangulWordSet.applyPattern(UNICODE_STRING_SIMPLE("[\\uac00-\\ud7a3]"), status);
    fHanWordSet.applyPattern(UNICODE_STRING_SIMPLE("[:Han:]"), status);
    fKatakanaWordSet.applyPattern(UNICODE_STRING_SIMPLE("[[:Katakana:]\\uff9e\\uff9f]"), status);
    fHiraganaWordSet.applyPattern(UNICODE_STRING_SIMPLE("[:Hiragana:]"), status);
    nfkcNorm2 = Normalizer2::getNFKCInstance(status);

    if (U_SUCCESS(status)) {
        // handle Korean and Japanese/Chinese using different dictionaries
        if (type == kKorean) {
            setCharacters(fHangulWordSet);
        } else { //Chinese and Japanese
            UnicodeSet cjSet;
            cjSet.addAll(fHanWordSet);
            cjSet.addAll(fKatakanaWordSet);
            cjSet.addAll(fHiraganaWordSet);
            cjSet.add(0xFF70); // HALFWIDTH KATAKANA-HIRAGANA PROLONGED SOUND MARK
            cjSet.add(0x30FC); // KATAKANA-HIRAGANA PROLONGED SOUND MARK
            setCharacters(cjSet);
        }
    }
}

CjkBreakEngine::~CjkBreakEngine(){
    delete fDictionary;
}

// The katakanaCost values below are based on the length frequencies of all
// katakana phrases in the dictionary
static const int kMaxKatakanaLength = 8;
static const int kMaxKatakanaGroupLength = 20;
static const uint32_t maxSnlp = 255;

static inline uint32_t getKatakanaCost(int wordLength){
    //TODO: fill array with actual values from dictionary!
    static const uint32_t katakanaCost[kMaxKatakanaLength + 1]
                                       = {8192, 984, 408, 240, 204, 252, 300, 372, 480};
    return (wordLength > kMaxKatakanaLength) ? 8192 : katakanaCost[wordLength];
}

static inline bool isKatakana(uint16_t value) {
    return (value >= 0x30A1u && value <= 0x30FEu && value != 0x30FBu) ||
            (value >= 0xFF66u && value <= 0xFF9fu);
}

// A very simple helper class to streamline the buffer handling in
// divideUpDictionaryRange. 
template<class T, size_t N>
class AutoBuffer {
public:
    AutoBuffer(size_t size) : buffer(stackBuffer), capacity(N) {
        if (size > N) {
            buffer = reinterpret_cast<T*>(uprv_malloc(sizeof(T)*size));
            capacity = size;
        }
    }
    ~AutoBuffer() {
        if (buffer != stackBuffer)
            uprv_free(buffer);
    }

    T* elems() {
        return buffer;
    }

    const T& operator[] (size_t i) const {
        return buffer[i];
    }

    T& operator[] (size_t i) {
        return buffer[i];
    }

    // resize without copy
    void resize(size_t size) {
        if (size <= capacity)
            return;
        if (buffer != stackBuffer)
            uprv_free(buffer);
        buffer = reinterpret_cast<T*>(uprv_malloc(sizeof(T)*size));
        capacity = size;
    }

private:
    T stackBuffer[N];
    T* buffer;
    AutoBuffer();
    size_t capacity;
};


// Function for accessing internal utext flags.
//   Replicates an internal UText function.

static inline int32_t utext_i32_flag(int32_t bitIndex) {
    return (int32_t)1 << bitIndex;
}

/*
 * @param text A UText representing the text
 * @param rangeStart The start of the range of dictionary characters
 * @param rangeEnd The end of the range of dictionary characters
 * @param foundBreaks Output of C array of int32_t break positions, or 0
 * @return The number of breaks found
 */
int32_t 
CjkBreakEngine::divideUpDictionaryRange( UText *inText,
        int32_t rangeStart,
        int32_t rangeEnd,
        UStack &foundBreaks ) const {
    if (rangeStart >= rangeEnd) {
        return 0;
    }

    // UnicodeString version of input UText, NFKC normalized in necessary.
    UnicodeString *inString;

    // inputMap[inStringIndex] = corresponding native index from UText inText.
    // If NULL then mapping is 1:1
    UVector32     *inputMap    = NULL;

    UErrorCode     status      = U_ZERO_ERROR;


    // if UText has the input string as one contiguous UTF-16 chunk
    if ((inText->providerProperties & utext_i32_flag(UTEXT_PROVIDER_STABLE_CHUNKS)) &&
         inText->chunkNativeStart <= rangeStart &&
         inText->chunkNativeLimit >= rangeEnd   &&
         inText->nativeIndexingLimit >= rangeEnd - inText->chunkNativeStart) {
         
        // Input UTtxt is in one contiguous UTF-16 chunk.
        // Use Read-only aliasing UnicodeString constructor on it.
        inString = new UnicodeString(FALSE, 
                              inText->chunkContents + rangeStart - inText->chunkNativeStart,
                              rangeEnd - rangeStart);
    } else {
        // Copy the text from the original inText (UText) to inString (UnicodeString).
        utext_setNativeIndex(inText, rangeStart);
        int32_t limit = rangeEnd;
        U_ASSERT(limit <= utext_nativeLength(inText));
        if (limit > utext_nativeLength(inText)) {
            limit = utext_nativeLength(inText);
        }
        inString = new UnicodeString;
        inputMap = new UVector32(status);
        while (utext_getNativeIndex(inText) < limit) {
            int32_t nativePosition = utext_getNativeIndex(inText);
            UChar32 c = utext_next32(inText);
            U_ASSERT(c != U_SENTINEL);
            inString->append(utext_next32(inText));
            while (inputMap->size() < inString->length()) {
                inputMap->addElement(nativePosition, status);
            }
        }
        inputMap->addElement(limit, status);
    }


    if (!nfkcNorm2->isNormalized(*inString, status)) {
        UnicodeString *normalizedInput = new UnicodeString();   
        //  normalizedMap[normalizedInput position] ==  original UText position.
        UVector32 *normalizedMap = new UVector32(status);
        if (U_FAILURE(status)) {
            return 0;
        }
        
        UnicodeString fragment;
        UnicodeString normalizedFragment;
        for (int32_t srcI = 0; srcI < inString->length();) {                 // Once per normalization chunk
            fragment.remove();
            int32_t fragmentStartI = srcI;
            UChar32 c = inString->char32At(srcI);
            for (;;) {
                fragment.append(c);
                srcI = inString->moveIndex32(srcI, 1);
                if (srcI == inString->length()) {
                    break;
                }
                c = inString->char32At(srcI);
                if (nfkcNorm2->hasBoundaryBefore(c)) {
                    break;
                }
            }
            nfkcNorm2->normalize(fragment, normalizedFragment, status);
            normalizedInput->append(normalizedFragment);

            // Map every position in the normalized chunk to the start of the chunk
            //   in the original input.
            int32_t fragmentOriginalStart = inputMap? inputMap->elementAti(fragmentStartI) : fragmentStartI;
            while (normalizedMap->size() < normalizedInput->length()) {
                normalizedMap->addElement(fragmentOriginalStart, status);
                if (U_FAILURE(status)) {
                    break;
                }
            }
        }
        delete inputMap;
        inputMap = normalizedMap;
        delete inString;
        inString = normalizedInput;
    }

    int32_t numCodePts = inString->countChar32();
    if (numCodePts != inString->length()) {
        // There are supplementary characters in the input.
        // The dictionary will produce boundary positions in terms of code point indexes,
        //   not in terms of code unit string indexes.
        // Use the inputMap mechanism to take care of this in addition to indexing differences
        //    from normalization and/or UTF-8 input.
        UBool hadExistingMap = (inputMap != NULL);
        if (!hadExistingMap) {
            inputMap = new UVector32(status);
        }
        int32_t cpIdx = 0;
        for (int32_t cuIdx = 0; ; cuIdx = inString->moveIndex32(cuIdx, 1)) {
            U_ASSERT(cuIdx >= cpIdx);
            if (hadExistingMap) {
                inputMap->setElementAt(inputMap->elementAti(cuIdx), cpIdx);
            } else {
                inputMap->addElement(cuIdx, status);
            }
            cpIdx++;
            if (cuIdx == inString->length()) {
               break;
            }
        }
    }
                
    // bestSnlp[i] is the snlp of the best segmentation of the first i
    // code points in the range to be matched.
    UVector32 bestSnlp(numCodePts + 1, status);
    bestSnlp.addElement(0, status);
    for(int i = 1; i <= numCodePts; i++) {
        bestSnlp.addElement(kuint32max, status);
    }


    // prev[i] is the index of the last CJK code point in the previous word in 
    // the best segmentation of the first i characters.
    UVector32 prev(numCodePts + 1, status);
    for(int32_t i = 0; i <= numCodePts; i++){
        prev.addElement(-1, status);
    }

    const int32_t maxWordSize = 20;
    UVector32 values(numCodePts, status);
    values.setSize(numCodePts);
    UVector32 lengths(numCodePts, status);
    lengths.setSize(numCodePts);

    // TODO: dump this UText, switch dictionary->matches() to work on UnicodeString.
    UText fu = UTEXT_INITIALIZER;
    utext_openUnicodeString(&fu, inString, &status);

    // Dynamic programming to find the best segmentation.

    // In outer loop, i  is the code point index,
    //                ix is the corresponding string (code unit) index.
    //    They differ when the string contains supplementary characters.
    int32_t ix = 0;
    for (int32_t i = 0;  i < numCodePts;  ++i, ix = inString->moveIndex32(ix, 1)) {
        if ((uint32_t)bestSnlp.elementAti(i) == kuint32max) {
            continue;
        }

        int32_t count;
        // limit maximum word length matched to size of current substring
        int32_t maxSearchLength = (i + maxWordSize < numCodePts) ? maxWordSize : (numCodePts - i);

        // TODO: utext vs. UnicodeString. Change matches() function.
        //       pass lengths and values as UVector32. Eliminates count, limit.
        utext_setNativeIndex(&fu, ix);

        fDictionary->matches(&fu, maxSearchLength, lengths.getBuffer(), count, maxSearchLength, values.getBuffer());

        // if there are no single character matches found in the dictionary 
        // starting with this charcter, treat character as a 1-character word 
        // with the highest value possible, i.e. the least likely to occur.
        // Exclude Korean characters from this treatment, as they should be left
        // together by default.
        if ((count == 0 || lengths.elementAti(0) != 1) &&
                !fHangulWordSet.contains(inString->char32At(ix))) {
            values.setElementAt(maxSnlp, count);   // 255
            lengths.setElementAt(1, count++);
        }

        for (int32_t j = 0; j < count; j++) {
            uint32_t newSnlp = (uint32_t)bestSnlp.elementAti(i) + (uint32_t)values.elementAti(j);
            int32_t ln_j_i = lengths.elementAti(j) + i;
            if (newSnlp < (uint32_t)bestSnlp.elementAti(ln_j_i)) {
                bestSnlp.setElementAt(newSnlp, ln_j_i);
                prev.setElementAt(i, ln_j_i);
            }
        }

        // In Japanese,
        // Katakana word in single character is pretty rare. So we apply
        // the following heuristic to Katakana: any continuous run of Katakana
        // characters is considered a candidate word with a default cost
        // specified in the katakanaCost table according to its length.

        bool is_prev_katakana = false;
        bool is_katakana = isKatakana(inString->char32At(ix));
        int32_t katakanaRunLength = 1;
        if (!is_prev_katakana && is_katakana) {
            int32_t j = inString->moveIndex32(ix, 1);
            // Find the end of the continuous run of Katakana characters
            while (j < inString->length() && katakanaRunLength < kMaxKatakanaGroupLength &&
                    isKatakana(inString->char32At(j))) {
                j = inString->moveIndex32(j, 1);
                katakanaRunLength++;
            }
            if (katakanaRunLength < kMaxKatakanaGroupLength) {
                uint32_t newSnlp = bestSnlp.elementAti(i) + getKatakanaCost(katakanaRunLength);
                if (newSnlp < (uint32_t)bestSnlp.elementAti(j)) {
                    bestSnlp.setElementAt(newSnlp, j);
                    prev.setElementAt(i, i+katakanaRunLength);  // prev[j] = i;
                }
            }
        }
        is_prev_katakana = is_katakana;
    }
    utext_close(&fu);   // TODO: dump this.

    // Start pushing the optimal offset index into t_boundary (t for tentative).
    // prev[numCodePts] is guaranteed to be meaningful.
    // We'll first push in the reverse order, i.e.,
    // t_boundary[0] = numCodePts, and afterwards do a swap.
    UVector32 t_boundary(numCodePts+1, status);

    int numBreaks = 0;
    // No segmentation found, set boundary to end of range
    if ((uint32_t)bestSnlp.elementAti(numCodePts) == kuint32max) {
        t_boundary.addElement(numCodePts, status);
        numBreaks++;
    } else {
        for (int32_t i = numCodePts; i > 0; i = prev.elementAti(i)) {
            t_boundary.addElement(i, status);
            numBreaks++;
        }
        U_ASSERT(prev.elementAti(t_boundary.elementAti(numBreaks - 1)) == 0);
    }

    // Add a break for the start of the dictionary range if there is not one
    // there already.
    if (foundBreaks.size() == 0 || foundBreaks.peeki() < rangeStart) {
        t_boundary.addElement(rangeStart, status);
        numBreaks++;
    }

    // Now that we're done, convert positions in t_boundary[] (indices in 
    // the normalized input string) back to indices in the original input UText
    // while reversing t_boundary and pushing values to foundBreaks.
    for (int i = numBreaks-1; i >= 0; i--) {
        int32_t cpPos = t_boundary.elementAti(i);
        int32_t utextPos = rangeStart + (inputMap ? inputMap->elementAti(cpPos) : cpPos);
        foundBreaks.push(utextPos, status);
    }

    delete inString;
    delete inputMap;
    return numBreaks;
}
#endif

U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_BREAK_ITERATION */
