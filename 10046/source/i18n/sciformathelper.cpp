/*
**********************************************************************
* Copyright (c) 2014, International Business Machines
* Corporation and others.  All Rights Reserved.
**********************************************************************
*/
#include "unicode/utypes.h"
#include "unicode/sciformathelper.h"
#include "unicode/dcfmtsym.h"
#include "unicode/fpositer.h"

#define LENGTHOF(array) (int32_t)(sizeof(array)/sizeof((array)[0]))

// TODO: Add U_DRAFT_API directives.
// TODO: Add U_FORMATTING directives

U_NAMESPACE_BEGIN

static UChar kExponentDigits[] = {0x2070, 0xB9, 0xB2, 0xB3, 0x2074, 0x2075, 0x2076, 0x2077, 0x2078, 0x2079};

static UnicodeString getMultiplicationSymbol(const DecimalFormatSymbols &dfs) {
    static UChar multSign = 0xD7;
    return UnicodeString(FALSE, &multSign, 1);
}

SciFormatHelper::SciFormatHelper(
        const DecimalFormatSymbols &dfs, UErrorCode &status) : fPreExponent() {
    if (U_FAILURE(status)) {
        return;
    }
    fPreExponent.append(getMultiplicationSymbol(dfs));
    fPreExponent.append(dfs.getSymbol(DecimalFormatSymbols::kOneDigitSymbol));
    fPreExponent.append(dfs.getSymbol(DecimalFormatSymbols::kZeroDigitSymbol));
}

SciFormatHelper::SciFormatHelper(
        const SciFormatHelper &other) : fPreExponent(other.fPreExponent) {
}

SciFormatHelper &SciFormatHelper::operator=(const SciFormatHelper &other) {
    if (this == &other) {
        return *this;
    }
    fPreExponent = other.fPreExponent;
    return *this;
}

SciFormatHelper::~SciFormatHelper() {
}

UnicodeString &SciFormatHelper::insetMarkup(
        const UnicodeString &s,
        FieldPositionIterator &fpi,
        const UnicodeString &beginMarkup,
        const UnicodeString &endMarkup,
        UnicodeString &result,
        UErrorCode &status) const {
    FieldPosition fp;
    int32_t copyFromOffset = 0;
    while (fpi.next(fp)) {
        switch (fp.getField()) {
        case UNUM_EXPONENT_SYMBOL_FIELD:
            result.append(s, copyFromOffset, fp.getBeginIndex() - copyFromOffset);
            copyFromOffset = fp.getEndIndex();
            result.append(fPreExponent);
            result.append(beginMarkup);
            break;
        case UNUM_EXPONENT_FIELD:
            result.append(s, copyFromOffset, fp.getEndIndex() - copyFromOffset);
            copyFromOffset = fp.getEndIndex();
            result.append(endMarkup);
            break;
        default:
            break;
        }
    }
    result.append(s, copyFromOffset, s.length() - copyFromOffset);
    return result;
}

static UBool copyAsSuperscript(
        const UnicodeString &s,
        int32_t beginIndex,
        int32_t endIndex,
        UnicodeString &result,
        UErrorCode &status) {
    if (U_FAILURE(status)) {
        return FALSE;
    }
    for (int32_t i = beginIndex; i < endIndex; ++i) {
        if (s[i] >= 0x30 && s[i] <= 0x39) {
            result.append(kExponentDigits[s[i] - 0x30]);
        } else {
            status = U_INVALID_CHAR_FOUND;
            return FALSE;
        }
    }
    return TRUE;
}

static UBool isMinusSign(UChar ch) {
    // TODO: revisit this.
    return (ch == 0x2D);
}

UnicodeString &SciFormatHelper::toSuperscriptExponentDigits(
        const UnicodeString &s,
        FieldPositionIterator &fpi,
        UnicodeString &result,
        UErrorCode &status) const {
    FieldPosition fp;
    int32_t copyFromOffset = 0;
    while (fpi.next(fp)) {
        switch (fp.getField()) {
        case UNUM_EXPONENT_SYMBOL_FIELD:
            result.append(s, copyFromOffset, fp.getBeginIndex() - copyFromOffset);
            copyFromOffset = fp.getEndIndex();
            result.append(fPreExponent);
            break;
        case UNUM_EXPONENT_SIGN_FIELD:
            {
                int32_t beginIndex = fp.getBeginIndex();
                int32_t endIndex = fp.getEndIndex();
                if (endIndex - beginIndex == 1 && isMinusSign(s[beginIndex])) {
                    result.append(s, copyFromOffset, beginIndex - copyFromOffset);
                    result.append(0x207B);
                } else {
                    status = U_INVALID_CHAR_FOUND;
                    return result;
                }
                copyFromOffset = endIndex;
            }
            break;
        case UNUM_EXPONENT_FIELD:
            result.append(s, copyFromOffset, fp.getBeginIndex() - copyFromOffset);
            if (!copyAsSuperscript(
                    s, fp.getBeginIndex(), fp.getEndIndex(), result, status)) {
              return result;
            }
            copyFromOffset = fp.getEndIndex();
            break;
        default:
            break;
        }
    }
    result.append(s, copyFromOffset, s.length() - copyFromOffset);
    return result;
}

U_NAMESPACE_END