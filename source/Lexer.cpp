#include <cstdint>
#include <memory>
#include <string>
#include <filesystem>
#include <unordered_map>
#include <deque>
#include <set>

#include "BumpAllocator.h"
#include "Buffer.h"
#include "StringRef.h"
#include "Diagnostics.h"
#include "FileTracker.h"
#include "Token.h"
#include "Lexer.h"
#include "CharInfo.h"
#include "SyntaxFacts.h"

namespace {

const int MaxMantissaDigits = 18;
const int MaxExponent = 511;

const double powersOf10[] = {
    10.0,
    100.0,
    1.0e4,
    1.0e8,
    1.0e16,
    1.0e32,
    1.0e64,
    1.0e128,
    1.0e256
};

template<typename T>
slang::ArrayRef<T> copyArray(slang::BumpAllocator& alloc, const slang::Buffer<T>& buffer) {
    uint32_t count = buffer.count();
    if (count == 0)
        return slang::ArrayRef<T>(nullptr, 0);

    const T* source = buffer.cbegin();
    T* dest = reinterpret_cast<T*>(alloc.allocate(count * sizeof(T)));
    for (uint32_t i = 0; i < count; i++)
        new (&dest[i]) T(*source++);
    return slang::ArrayRef<T>(dest, count);
}

bool composeDouble(double fraction, int exp, double& result) {
    bool neg = false;
    if (exp < 0) {
        neg = true;
        exp = -exp;
    }

    if (exp > MaxExponent)
        exp = MaxExponent;

    double dblExp = 1.0;
    for (auto d = powersOf10; exp != 0; exp >>= 1, d++) {
        if (exp & 0x1)
            dblExp *= *d;
    }

    if (neg)
        fraction /= dblExp;
    else
        fraction *= dblExp;

    result = fraction;
    return std::isfinite(result);
}

} // anonymous namespace

namespace slang {

Lexer::Lexer(FileID file, StringRef source, BumpAllocator& alloc, Diagnostics& diagnostics) :
    stringBuffer(1024),
    triviaBuffer(32),
    alloc(alloc),
    diagnostics(diagnostics),
    sourceBuffer(source.begin()),
    sourceEnd(source.end()),
    marker(nullptr),
    file(file),
    mode(LexingMode::Normal) {

    // string needs to be non-null and null terminated
    ASSERT(source);
    ASSERT(source.isNullTerminated());

    // detect BOMs so we can give nice errors for invaild encoding
    if (source.length() >= 2) {
        const unsigned char* ubuf = reinterpret_cast<const unsigned char*>(sourceBuffer);
        if ((ubuf[0] == 0xFF && ubuf[1] == 0xFE) ||
            (ubuf[0] == 0xFE && ubuf[1] == 0xFF)) {
            addError(DiagCode::UnicodeBOM);
            advance(2);
        }
        else if (source.length() >= 3) {
            if (ubuf[0] == 0xEF &&
                ubuf[1] == 0xBB &&
                ubuf[2] == 0xBF) {
                addError(DiagCode::UnicodeBOM);
                advance(3);
            }
        }
    }
}

Token* Lexer::lex() {
    ASSERT(!reallyAtEnd());

    // lex leading trivia
    triviaBuffer.clear();
    lexTrivia();

    /*if (lexTrivia()) {
        // we found a directive that requires some kind of expansion (`include, macro usage)
        // let the preprocessor figure out the next token and attach all of our trivia to it
        Token* token = preprocessor.next();
        ASSERT(token);

        // stitch together our trivia with the token from the other buffer
        for (auto& tr : token->trivia)
            triviaBuffer.append(tr);

        // end of file is a special case; it means the `include or macro was empty, so just
        // fall through in that case and lex the next token in our own file
        if (token->kind != TokenKind::EndOfFile) {
            // build a merged token with the correct trivia
            return alloc.emplace<Token>(token->kind, token->getDataPtr(), copyArray(alloc, triviaBuffer));
        }
    }*/

    // lex the next token
    mark();
    void* data = nullptr;
    TokenKind kind = lexToken(&data);

    return alloc.emplace<Token>(kind, data, copyArray(alloc, triviaBuffer));
}

TokenKind Lexer::lexToken(void** extraData) {
    char c = peek();
    advance();
    switch (c) {
        case '\0':
            // check if we're not really at the end; can't use reallyAtEnd() here because
            // we've already advanced()
            if (sourceBuffer <= sourceEnd) {
                addError(DiagCode::EmbeddedNull);
                *extraData = alloc.emplace<IdentifierInfo>(lexeme(), IdentifierType::Unknown);
                return TokenKind::Unknown;
            }
            return TokenKind::EndOfFile;
        case '!':
            if (consume('=')) {
                switch (peek()) {
                    case '=': advance(); return TokenKind::ExclamationDoubleEquals;
                    case '?': advance(); return TokenKind::ExclamationEqualsQuestion;
                    default: return TokenKind::ExclamationEquals;
                }
            }
            return TokenKind::Exclamation;
        case '"':
            *extraData = lexStringLiteral();
            return TokenKind::StringLiteral;
        case '#':
            switch (peek()) {
                case '#': advance(); return TokenKind::DoubleHash;
                case '-':
                    if (peek(1) == '#') {
                        advance(2);
                        return TokenKind::HashMinusHash;
                    }
                    // #- isn't a token, so just return a hash
                    return TokenKind::Hash;
                case '=':
                    if (peek(1) == '#') {
                        advance(2);
                        return TokenKind::HashEqualsHash;
                    }
                    // #= isn't a token, so just return a hash
                    return TokenKind::Hash;
            }
            return TokenKind::Hash;
        case '$': return lexDollarSign(extraData);
        case '%':
            if (consume('='))
                return TokenKind::PercentEqual;
            return TokenKind::Percent;
        case '&':
            switch (peek()) {
                case '&':
                    advance();
                    if (consume('&'))
                        return TokenKind::TripleAnd;
                    else
                        return TokenKind::DoubleAnd;
                case '=': advance(); return TokenKind::AndEqual;
            }
            return TokenKind::And;
        case '\'':
            // either an unsized numeric literal, or a '{ range open sequence
            if (consume('{'))
                return TokenKind::ApostropheOpenBrace;

            *extraData = lexUnsizedNumericLiteral();
            return TokenKind::IntegerLiteral;
        case '(':
            if (consume('*'))
                return TokenKind::OpenParenthesisStar;
            else
                return TokenKind::OpenParenthesis;
        case ')': return TokenKind::CloseParenthesis;
        case '*':
            switch (peek()) {
                case '*': advance(); return TokenKind::DoubleStar;
                case '=': advance(); return TokenKind::StarEqual;
                case '>': advance(); return TokenKind::StarArrow;
                case ')': advance(); return TokenKind::StarCloseParenthesis;
                case ':':
                    if (peek(1) == ':' && peek(2) == '*') {
                        advance(3);
                        return TokenKind::StarDoubleColonStar;
                    }
                    return TokenKind::Star;
            }
            return TokenKind::Star;
        case '+':
            switch (peek()) {
                case '+': advance(); return TokenKind::DoublePlus;
                case '=': advance(); return TokenKind::PlusEqual;
                case ':': advance(); return TokenKind::PlusColon;
            }
            return TokenKind::Plus;
        case ',': return TokenKind::Comma;
        case '-':
            switch (peek()) {
                case '-': advance(); return TokenKind::DoubleMinus;
                case '=': advance(); return TokenKind::MinusEqual;
                case ':': advance(); return TokenKind::MinusColon;
                case '>':
                    advance();
                    if (consume('>'))
                        return TokenKind::MinusDoubleArrow;
                    else
                        return TokenKind::MinusArrow;
            }
            return TokenKind::Minus;
        case '.':
            if (consume('*'))
                return TokenKind::DotStar;
            else
                return TokenKind::Dot;
        case '/':
            if (consume('='))
                return TokenKind::SlashEqual;
            else
                return TokenKind::Slash;
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7':
        case '8': case '9':
            // back up so that lexNumericLiteral can look at this digit again
            sourceBuffer--;
            return lexNumericLiteral(extraData);
        case ':':
            switch (peek()) {
                case '=': advance(); return TokenKind::ColonEquals;
                case '/': advance(); return TokenKind::ColonSlash;
                case ':': advance(); return TokenKind::DoubleColon;
            }
            return TokenKind::Colon;
        case ';': return TokenKind::Semicolon;
        case '<':
            switch (peek()) {
                case '=': advance(); return TokenKind::LessThanEquals;
                case '-':
                    if (peek(1) == '>') {
                        advance(2);
                        return TokenKind::LessThanMinusArrow;
                    }
                    return TokenKind::LessThan;
                case '<':
                    advance();
                    switch (peek()) {
                        case '<':
                            if (peek(1) == '=') {
                                advance(2);
                                return TokenKind::TripleLeftShiftEqual;
                            }
                            else {
                                advance();
                                return TokenKind::TripleLeftShift;
                            }
                        case '=': advance(); return TokenKind::LeftShiftEqual;
                    }
                    return TokenKind::LeftShift;
            }
            return TokenKind::LessThan;
        case '=':
            switch (peek()) {
                case '=':
                    advance();
                    switch (peek()) {
                        case '=': advance(); return TokenKind::TripleEquals;
                        case '?': advance(); return TokenKind::DoubleEqualsQuestion;
                    }
                    return TokenKind::DoubleEquals;
                case '>': advance(); return TokenKind::EqualsArrow;
            }
            return TokenKind::Equals;
        case '>':
            switch (peek()) {
                case '=': advance(); return TokenKind::GreaterThanEquals;
                case '>':
                    advance();
                    switch (peek()) {
                        case '>':
                            if (peek(1) == '=') {
                                advance(2);
                                return TokenKind::TripleRightShiftEqual;
                            }
                            else {
                                advance();
                                return TokenKind::TripleRightShift;
                            }
                        case '=': advance(); return TokenKind::RightShiftEqual;
                    }
                    return TokenKind::RightShift;
            }
            return TokenKind::GreaterThan;
        case '?': return TokenKind::Question;
        case '@':
            if (consume('@'))
                return TokenKind::DoubleAt;
            else
                return TokenKind::At;
        case 'A': case 'B': case 'C': case 'D':
        case 'E': case 'F': case 'G': case 'H':
        case 'I': case 'J': case 'L': case 'K':
        case 'M': case 'N': case 'O': case 'P':
        case 'Q': case 'R': case 'S': case 'T':
        case 'U': case 'V': case 'W': case 'X':
        case 'Y': case 'Z':
        case 'a': case 'b': case 'c': case 'd':
        case 'e': case 'f': case 'g': case 'h':
        case 'i': case 'j': case 'k': case 'l':
        case 'm': case 'n': case 'o': case 'p':
        case 'q': case 'r': case 's': case 't':
        case 'u': case 'v': case 'w': case 'x':
        case 'y': case 'z':
        case '_': {
            scanIdentifier();
            *extraData = alloc.emplace<IdentifierInfo>(lexeme(), IdentifierType::Normal);
            return TokenKind::Identifier;
        }
        case '[': return TokenKind::OpenBracket;
        case '\\': return lexEscapeSequence(extraData);
        case ']': return TokenKind::CloseBracket;
        case '^':
            switch (peek()) {
                case '~': advance(); return TokenKind::XorTilde;
                case '=': advance(); return TokenKind::XorEqual;
            }
            return TokenKind::Xor;
        case '`':
            switch (peek()) {
                case '"': advance(); return TokenKind::MacroQuote;
                case '`': advance(); return TokenKind::MacroPaste;
                case '\\':
                    if (peek(1) == '`' && peek(2) == '"') {
                        advance(3);
                        return TokenKind::MacroEscapedQuote;
                    }
                    return lexDirective(extraData);
            }
            return lexDirective(extraData);
        case '{': return TokenKind::OpenBrace;
        case '|':
            switch (peek()) {
                case '|': advance(); return TokenKind::DoubleOr;
                case '-':
                    if (peek(1) == '>') {
                        advance(2);
                        return TokenKind::OrMinusArrow;
                    }
                    return TokenKind::Or;
                case '=':
                    if (peek(1) == '>') {
                        advance(2);
                        return TokenKind::OrEqualsArrow;
                    }
                    else {
                        advance();
                        return TokenKind::OrEqual;
                    }
            }
            return TokenKind::Or;
        case '}': return TokenKind::CloseBrace;
        case '~':
            switch (peek()) {
                case '&': advance(); return TokenKind::TildeAnd;
                case '|': advance(); return TokenKind::TildeOr;
                case '^': advance(); return TokenKind::TildeXor;
            }
            return TokenKind::Tilde;
        default:
            if (isASCII(c))
                addError(DiagCode::NonPrintableChar);
            else {
                // skip over UTF-8 sequences
                advance(utf8SeqBytes(c));
                addError(DiagCode::UTF8Char);
            }
            *extraData = alloc.emplace<IdentifierInfo>(lexeme(), IdentifierType::Unknown);
            return TokenKind::Unknown;
    }
}

void Lexer::scanIdentifier() {
    while (true) {
        char c = peek();
        if (isAlphaNumeric(c) || c == '_' || c == '$')
            advance();
        else
            return;
    }
}

StringLiteralInfo* Lexer::lexStringLiteral() {
    stringBuffer.clear();

    while (true) {
        char c = peek();
        if (c == '\\') {
            advance();
            c = peek();
            advance();

            uint32_t charCode;
            switch (c) {
                case 'n': stringBuffer.append('\n'); break;
                case 't': stringBuffer.append('\t'); break;
                case '\\': stringBuffer.append('\\'); break;
                case '"': stringBuffer.append('"'); break;
                case 'v': stringBuffer.append('\v'); break;
                case 'f': stringBuffer.append('\f'); break;
                case 'a': stringBuffer.append('\a'); break;
                case '\n': break;
                case '\r': consume('\n'); break;
                case '0': case '1': case '2': case '3':
                case '4': case '5': case '6': case '7':
                    // octal character code
                    charCode = getDigitValue(c);
                    if (isOctalDigit(c = peek())) {
                        advance();
                        charCode = (charCode * 8) + getDigitValue(c);
                        if (isOctalDigit(c = peek())) {
                            advance();
                            charCode = (charCode * 8) + getDigitValue(c);
                            if (charCode > 255) {
                                addError(DiagCode::OctalEscapeCodeTooBig);
                                break;
                            }
                        }
                    }
                    stringBuffer.append((char)charCode);
                    break;
                case 'x':
                    c = peek();
                    advance();
                    if (!isHexDigit(c)) {
                        addError(DiagCode::InvalidHexEscapeCode);
                        stringBuffer.append(c);
                    }
                    else {
                        charCode = getHexDigitValue(c);
                        if (isHexDigit(c = peek())) {
                            advance();
                            charCode = (charCode * 16) + getHexDigitValue(c);
                        }
                        stringBuffer.append((char)charCode);
                    }
                    break;
                default:
                    addError(DiagCode::UnknownEscapeCode);
                    stringBuffer.append(c);
                    break;
            }
        }
        else if (c == '"') {
            advance();
            break;
        }
        else if (isNewline(c)) {
            addError(DiagCode::NewlineInStringLiteral);
            break;
        }
        else if (c == '\0') {
            if (reallyAtEnd()) {
                addError(DiagCode::UnterminatedStringLiteral);
                break;
            }

            // otherwise just error and ignore
            addError(DiagCode::EmbeddedNull);
        }
        else {
            advance();
            stringBuffer.append(c);
        }
    }

    StringRef niceText = StringRef(stringBuffer.begin(), stringBuffer.count()).intern(alloc);
    return alloc.emplace<StringLiteralInfo>(lexeme(), niceText);
}

TokenKind Lexer::lexEscapeSequence(void** extraData) {
    char c = peek();
    if (isWhitespace(c) || c == '\0') {
        addError(DiagCode::EscapedWhitespace);
        *extraData = alloc.emplace<IdentifierInfo>(lexeme(), IdentifierType::Unknown);
        return TokenKind::Unknown;
    }

    while (isPrintable(c)) {
        advance();
        c = peek();
        if (isWhitespace(c))
            break;
    }

    *extraData = alloc.emplace<IdentifierInfo>(lexeme(), IdentifierType::Escaped);
    return TokenKind::Identifier;
}

TokenKind Lexer::lexDollarSign(void** extraData) {
    scanIdentifier();

    // if length is 1, we just have a dollar sign operator
    if (lexemeLength() == 1)
        return TokenKind::Dollar;

    // otherwise, we have a system identifier
    *extraData = alloc.emplace<IdentifierInfo>(lexeme(), IdentifierType::System);
    return TokenKind::SystemIdentifier;
}

TokenKind Lexer::lexDirective(void** extraData) {
    scanIdentifier();

    // if length is 1, we just have a grave character on its own, which is an error
    if (lexemeLength() == 1) {
        addError(DiagCode::MisplacedDirectiveChar);
        *extraData = alloc.emplace<IdentifierInfo>(lexeme(), IdentifierType::Unknown);
        return TokenKind::Unknown;
    }

    auto directive = lexeme();
    TriviaKind type = getDirectiveKind(directive);
    *extraData = alloc.emplace<DirectiveInfo>(directive, type);

    // lexing behavior changes slightly depending on directives we see
    switch (type) {
        case TriviaKind::MacroUsage:
            return TokenKind::MacroUsage;
        case TriviaKind::IncludeDirective:
            mode = LexingMode::Include;
            break;
        default:
            mode = LexingMode::Directive;
            break;
    }
    return TokenKind::Directive;
}

TokenKind Lexer::lexNumericLiteral(void** extraData) {
    // skip over leading zeros
    char c;
    while ((c = peek()) == '0')
        advance();

    // scan past leading decimal digits; these might be the first part of
    // a fractional number, the size of a vector, or a plain unsigned integer
    uint64_t unsignedVal = 0;
    int digits = 0;
    c = scanUnsignedNumber(c, unsignedVal, digits);

    // whitespace normally ends a numeric literal, but it's allowed between
    // the size and the base specifier in vector literals, so check if that's what we have here
    int lookahead = findNextNonWhitespace();
    if (lookahead > 0 && peek(lookahead) == '\'') {
        advance(lookahead + 1);
        *extraData = lexVectorLiteral(unsignedVal);
        return TokenKind::IntegerLiteral;
    }

    switch (peek()) {
        case '\'':
            advance();
            *extraData = lexVectorLiteral(unsignedVal);
            return TokenKind::IntegerLiteral;
        case '.': {
            // fractional digits
            int decPoint = digits;
            advance();
            c = peek();
            if (!isDecimalDigit(c))
                addError(DiagCode::MissingFractionalDigits);

            c = scanUnsignedNumber(peek(), unsignedVal, digits);
            *extraData = lexRealLiteral(
                unsignedVal,
                decPoint,
                digits,
                c == 'e' || c == 'E'
            );
            return TokenKind::RealLiteral;
        }
        case 'e':
        case 'E':
            *extraData = lexRealLiteral(
                unsignedVal,
                digits,     // decimal point is after all digits
                digits,
                true        // yep, we have an exponent
            );
            return TokenKind::RealLiteral;
        default:
            // normal signed numeric literal; check for 32-bit overflow
            if (unsignedVal > INT32_MAX) {
                unsignedVal = INT32_MAX;
                addError(DiagCode::SignedLiteralTooLarge);
            }
            *extraData = alloc.emplace<NumericLiteralInfo>(lexeme(), (int32_t)unsignedVal);
            return TokenKind::IntegerLiteral;
    }
}

char Lexer::scanUnsignedNumber(char c, uint64_t& unsignedVal, int& digits) {
    while (true) {
        if (isDecimalDigit(c)) {
            // After 18 digits, stop caring. For normal integers, we're going to truncate
            // to 32-bits anyway. For reals, later digits won't have any effect on the result.
            if (digits < MaxMantissaDigits)
                unsignedVal = (unsignedVal * 10) + getDigitValue(c);
            digits++;
        }
        else if (c != '_')
            break;

        advance();
        c = peek();
    }
    return c;
}

NumericLiteralInfo* Lexer::lexRealLiteral(uint64_t value, int decPoint, int digits, bool exponent) {
    bool neg = false;
    uint64_t expVal = 0;

    if (exponent) {
        advance();

        // skip over leading zeros
        char c = peek();
        while ((c = peek()) == '0')
            advance();

        if (c == '+')
            advance();
        else if (c == '-') {
            neg = true;
            advance();
        }

        c = peek();
        if (!isDecimalDigit(c))
            addError(DiagCode::MissingExponentDigits);
        else {
            int unusedDigits = 0;
            scanUnsignedNumber(c, expVal, unusedDigits);
        }
    }

    int fracExp = decPoint - std::min(digits, MaxMantissaDigits);
    int exp;
    if (neg)
        exp = fracExp - int(expVal);
    else
        exp = fracExp + int(expVal);

    double result;
    if (!composeDouble(double(value), exp, result))
        addError(DiagCode::RealExponentTooLarge);

    return alloc.emplace<NumericLiteralInfo>(lexeme(), result);
}

NumericLiteralInfo* Lexer::lexVectorLiteral(uint64_t size64) {
    // error checking on the size, plus coerce to 32-bit
    uint32_t size32 = 0;
    if (size64 == 0)
        addError(DiagCode::IntegerSizeZero);
    else {
        if (size64 > UINT32_MAX) {
            size64 = UINT32_MAX;
            addError(DiagCode::IntegerSizeTooLarge);
        }
        size32 = (uint32_t)size64;
    }

    // check for signed specifier
    bool isSigned = false;
    char c = peek();
    if (c == 's' || c == 'S') {
        isSigned = true;
        advance();
        c = peek();
    }

    vectorBuilder.start(size32, isSigned);

    // next character needs to be the base
    switch (c) {
        case 'd':
        case 'D':
            advance();
            return lexVectorDigits<isDecimalDigit, getDigitValue>();
        case 'o':
        case 'O':
            advance();
            return lexVectorDigits<isOctalDigit, getDigitValue>();
        case 'h':
        case 'H':
            advance();
            return lexVectorDigits<isHexDigit, getHexDigitValue>();
        case 'b':
        case 'B':
            advance();
            return lexVectorDigits<isBinaryDigit, getDigitValue>();
        default:
            // error case
            addError(DiagCode::MissingVectorBase);
            return alloc.emplace<NumericLiteralInfo>(lexeme(), 0);
    }
}

NumericLiteralInfo* Lexer::lexUnsizedNumericLiteral() {
    vectorBuilder.startUnsized();
    char c = peek();
    switch (c) {
        case 'd':
        case 'D':
            advance();
            return lexVectorDigits<isDecimalDigit, getDigitValue>();
        case 'o':
        case 'O':
            advance();
            return lexVectorDigits<isOctalDigit, getDigitValue>();
        case 'h':
        case 'H':
            advance();
            return lexVectorDigits<isHexDigit, getHexDigitValue>();
        case 'b':
        case 'B':
            advance();
            return lexVectorDigits<isBinaryDigit, getDigitValue>();
        case '0':
        case '1':
            advance();
            return alloc.emplace<NumericLiteralInfo>(lexeme(), (logic_t)(uint8_t)getDigitValue(c));
        case 'x':
        case 'X':
            advance();
            return alloc.emplace<NumericLiteralInfo>(lexeme(), logic_t::x);
        case 'Z':
        case 'z':
            advance();
            return alloc.emplace<NumericLiteralInfo>(lexeme(), logic_t::z);
        default:
            // error case
            addError(DiagCode::InvalidUnsizedLiteral);
            return alloc.emplace<NumericLiteralInfo>(lexeme(), 0);
    }
}

template<bool(*IsDigitFunc)(char), uint32_t(*ValueFunc)(char)>
NumericLiteralInfo* Lexer::lexVectorDigits() {
    // skip leading whitespace
    int lookahead = findNextNonWhitespace();
    char c = peek(lookahead);
    if (!IsDigitFunc(c) && !isLogicDigit(c)) {
        addError(DiagCode::MissingVectorDigits);
        return alloc.emplace<NumericLiteralInfo>(lexeme(), 0);
    }

    advance(lookahead);

    while (true) {
        c = peek();
        switch (c) {
            case '_':
                break;
            case 'z':
            case 'Z':
            case '?':
                vectorBuilder.addDigit(logic_t::z);
                break;
            case 'x':
            case 'X':
                vectorBuilder.addDigit(logic_t::x);
                break;
            default:
                if (IsDigitFunc(c))
                    vectorBuilder.addDigit((char)ValueFunc(c));
                else
                    return alloc.emplace<NumericLiteralInfo>(lexeme(), vectorBuilder.toVector());
        }
        advance();
    }
}

bool Lexer::lexTrivia() {
    // this function returns true and stops lexing trivia if we find a newline while
    // in directive mode, since that requires an EndOfDirective token to be issued
    while (true) {
        mark();

        switch (peek()) {
            case ' ':
            case '\t':
            case '\v':
            case '\f':
                advance();
                scanWhitespace();
                break;
            case '/':
                switch (peek(1)) {
                    case '/':
                        advance(2);
                        scanLineComment();
                        break;
                    case '*':
                        advance(2);
                        if (scanBlockComment())
                            return true;
                        break;
                    default:
                        return false;
                }
                break;
            case '\r':
                advance();
                consume('\n');
                addTrivia(TriviaKind::EndOfLine);
                if (mode != LexingMode::Normal)
                    return true;
                break;
            case '\n':
                advance();
                addTrivia(TriviaKind::EndOfLine);
                if (mode != LexingMode::Normal)
                    return true;
                break;
            case '\\':
                // if we're lexing a directive, this might escape a newline
                if (mode == LexingMode::Normal || !isNewline(peek()))
                    return false;
                advance();
                break;
            default:
                return false;
        }
    }
}

void Lexer::scanWhitespace() {
    bool done = false;
    while (!done) {
        switch (peek()) {
            case ' ':
            case '\t':
            case '\v':
            case '\f':
                advance();
                break;
            default:
                done = true;
                break;
        }
    }

    addTrivia(TriviaKind::Whitespace);
}

void Lexer::scanLineComment() {
    while (true) {
        char c = peek();
        if (isNewline(c))
            break;

        if (c == '\0') {
            if (reallyAtEnd())
                break;
            
            // otherwise just error and ignore
            addError(DiagCode::EmbeddedNull);
        }
        advance();
    }

    addTrivia(TriviaKind::LineComment);
}

bool Lexer::scanBlockComment() {
    bool eod = false;
    while (true) {
        char c = peek();
        if (c == '\0') {
            if (reallyAtEnd()) {
                addError(DiagCode::UnterminatedBlockComment);
                break;
            }

            // otherwise just error and ignore
            addError(DiagCode::EmbeddedNull);
        }
        else if (c == '*' && peek(1) == '/') {
            advance(2);
            break;
        }
        else if (c == '/' && peek(1) == '*') {
            // nested block comments disallowed by the standard; ignore and continue
            advance(2);
            addError(DiagCode::NestedBlockComment);
        }
        else {
            advance();
            if (mode != LexingMode::Normal && isNewline(c)) {
                // found a newline in a block comment inside a directive; this is not allowed
                // we need to stop lexing trivia and issue an EndOfDirective token after this comment
                addError(DiagCode::SplitBlockCommentInDirective);
                mode = LexingMode::Normal;
                eod = true;
            }
        }
    }

    addTrivia(TriviaKind::BlockComment);
    return eod;
}

//void Lexer::lexIncludeFileName() {
//    bool systemPath = false;
//    char delim;
//
//    // next non-whitespace character needs to be " or <
//    int lookahead = findNextNonWhitespace();
//    switch (delim = peek(lookahead)) {
//        case '"': break;
//        case '<': systemPath = true;
//        default:
//            addError(DiagCode::ExpectedIncludeFileName);
//            return;
//    }
//
//    advance(lookahead + 1);
//    const char* startOfFileName = sourceBuffer;
//
//    while (true) {
//        char c = peek();
//        if (c == delim)
//            break;
//
//        if (c == '\0' || isNewline(c)) {
//            addError(DiagCode::ExpectedEndOfIncludeFileName);
//            return;
//        }
//
//        advance();
//    }
//
//    // inform the preprocessor about this include
//   // preprocessor.include(StringRef(startOfFileName, (uint32_t)(sourceBuffer - startOfFileName)), systemPath);
//}

int Lexer::findNextNonWhitespace() {
    int lookahead = 0;
    char c;
    while (isHorizontalWhitespace(c = peek(lookahead)))
        lookahead++;
    return lookahead;
}

void Lexer::addTrivia(TriviaKind kind) {
    triviaBuffer.emplace(kind, lexeme());
}

void Lexer::addError(DiagCode code) {
    diagnostics.add(SyntaxError(code, 0, 0));
}

StringRef Lexer::lexeme() {
    uint32_t length = lexemeLength();
    return StringRef(marker, length).intern(alloc);
}

} // namespace slang