#include <emmintrin.h>

#include <sstream>

#include <mysqlxx/Manip.h>

#include <DB/Core/Defines.h>
#include <DB/IO/ReadHelpers.h>


namespace DB
{


static void __attribute__((__noinline__)) throwAtAssertionFailed(const char * s, ReadBuffer & buf)
{
	std::stringstream message;
	message <<  "Cannot parse input: expected " << mysqlxx::escape << s;

	if (buf.eof())
		message << " at end of stream.";
	else
		message << " before: " << mysqlxx::escape << String(buf.position(), std::min(SHOW_CHARS_ON_SYNTAX_ERROR, buf.buffer().end() - buf.position()));

	throw Exception(message.str(), ErrorCodes::CANNOT_PARSE_INPUT_ASSERTION_FAILED);
}


void assertString(const char * s, ReadBuffer & buf)
{
	for (; *s; ++s)
	{
		if (buf.eof() || *buf.position() != *s)
			throwAtAssertionFailed(s, buf);
		++buf.position();
	}
}

void assertEOF(ReadBuffer & buf)
{
	if (!buf.eof())
		throwAtAssertionFailed("eof", buf);
}

void readString(String & s, ReadBuffer & buf)
{
	s = "";
	while (!buf.eof())
	{
		size_t bytes = 0;
		for (; buf.position() + bytes != buf.buffer().end(); ++bytes)
			if (buf.position()[bytes] == '\t' || buf.position()[bytes] == '\n')
				break;

		s.append(buf.position(), bytes);
		buf.position() += bytes;

		if (buf.position() != buf.buffer().end())
			return;
	}
}


/** Позволяет найти в куске памяти следующий символ \t, \n или \\.
  * Функция похожа на strpbrk, но со следующими отличиями:
  * - работает с любыми кусками памяти, в том числе, с нулевыми байтами;
  * - не требует нулевого байта в конце - в функцию передаётся конец данных;
  * - в случае, если не найдено, возвращает указатель на конец, а не NULL.
  *
  * Использует SSE2, что даёт прирост скорости примерно в 1.7 раза (по сравнению с тривиальным циклом)
  *  при парсинге типичного tab-separated файла со строками.
  * Можно было бы использовать SSE4.2, но он пока поддерживается не на всех наших серверах.
  * При парсинге файла с короткими строками, падения производительности нет.
  */
static inline const char * find_first_tab_lf_or_backslash(const char * begin, const char * end)
{
	static const char tab_chars[16] = {'\t', '\t', '\t', '\t', '\t', '\t', '\t', '\t', '\t', '\t', '\t', '\t', '\t', '\t', '\t', '\t'};
	static const char lf_chars[16]	= {'\n', '\n', '\n', '\n', '\n', '\n', '\n', '\n', '\n', '\n', '\n', '\n', '\n', '\n', '\n', '\n'};
	static const char bs_chars[16] 	= {'\\', '\\', '\\', '\\', '\\', '\\', '\\', '\\', '\\', '\\', '\\', '\\', '\\', '\\', '\\', '\\'};

	static const __m128i tab	= *reinterpret_cast<const __m128i *>(tab_chars);
	static const __m128i lf		= *reinterpret_cast<const __m128i *>(lf_chars);
	static const __m128i bs		= *reinterpret_cast<const __m128i *>(bs_chars);

	for (; (reinterpret_cast<ptrdiff_t>(begin) & 0x0F) && begin < end; ++begin)
		if (*begin == '\t' || *begin == '\n' || *begin == '\\')
			return begin;

	for (; begin + 15 < end; begin += 16)
	{
		__m128i bytes = *reinterpret_cast<const __m128i *>(begin);

		__m128i eq1 = _mm_cmpeq_epi8(bytes, tab);
		__m128i eq2 = _mm_cmpeq_epi8(bytes, lf);
		__m128i eq3 = _mm_cmpeq_epi8(bytes, bs);

		eq1 = _mm_or_si128(eq1, eq2);
		eq1 = _mm_or_si128(eq1, eq3);

		UInt16 bit_mask = _mm_movemask_epi8(eq1);

		if (bit_mask)
			return begin + __builtin_ctz(bit_mask);
	}

	for (; begin < end; ++begin)
		if (*begin == '\t' || *begin == '\n' || *begin == '\\')
			return begin;

	return end;
}


void readEscapedString(DB::String & s, DB::ReadBuffer & buf)
{
	s = "";
	while (!buf.eof())
	{
		const char * next_pos = find_first_tab_lf_or_backslash(buf.position(), buf.buffer().end());

		s.append(buf.position(), next_pos - buf.position());
		buf.position() += next_pos - buf.position();

		if (buf.position() == buf.buffer().end())
			continue;

		if (*buf.position() == '\t' || *buf.position() == '\n')
			return;

		if (*buf.position() == '\\')
		{
			++buf.position();
			if (buf.eof())
				throw Exception("Cannot parse escape sequence", DB::ErrorCodes::CANNOT_PARSE_ESCAPE_SEQUENCE);
			s += DB::parseEscapeSequence(*buf.position());
			++buf.position();
		}
	}
}


template <char quote>
static inline const char * find_first_quote_or_backslash(const char * begin, const char * end)
{
	static const char quote_chars[16] 	= {quote, quote, quote, quote, quote, quote, quote, quote, quote, quote, quote, quote, quote, quote, quote, quote};
	static const char bs_chars[16] 		= {'\\',  '\\',  '\\',  '\\',  '\\',  '\\',  '\\',  '\\',  '\\',  '\\',  '\\',  '\\',  '\\',  '\\',  '\\',  '\\' };

	static const __m128i quote_128	= *reinterpret_cast<const __m128i *>(quote_chars);
	static const __m128i bs_128		= *reinterpret_cast<const __m128i *>(bs_chars);

	for (; (reinterpret_cast<ptrdiff_t>(begin) & 0x0F) && begin < end; ++begin)
		if (*begin == quote || *begin == '\\')
			return begin;

	for (; begin + 15 < end; begin += 16)
	{
		__m128i bytes = *reinterpret_cast<const __m128i *>(begin);

		__m128i eq1 = _mm_cmpeq_epi8(bytes, quote_128);
		__m128i eq2 = _mm_cmpeq_epi8(bytes, bs_128);

		eq1 = _mm_or_si128(eq1, eq2);

		UInt16 bit_mask = _mm_movemask_epi8(eq1);

		if (bit_mask)
			return begin + __builtin_ctz(bit_mask);
	}

	for (; begin < end; ++begin)
		if (*begin == quote || *begin == '\\')
			return begin;

	return end;
}


template <char quote>
static void readAnyQuotedString(String & s, ReadBuffer & buf)
{
	s = "";

	if (buf.eof() || *buf.position() != quote)
		throw Exception("Cannot parse quoted string: expected opening quote",
			ErrorCodes::CANNOT_PARSE_QUOTED_STRING);
	++buf.position();

	while (!buf.eof())
	{
		const char * next_pos = find_first_quote_or_backslash<quote>(buf.position(), buf.buffer().end());

		s.append(buf.position(), next_pos - buf.position());
		buf.position() += next_pos - buf.position();
		
		if (buf.position() == buf.buffer().end())
			continue;

		if (*buf.position() == quote)
		{
			++buf.position();
			return;
		}

		if (*buf.position() == '\\')
		{
			++buf.position();
			if (buf.eof())
				throw Exception("Cannot parse escape sequence", ErrorCodes::CANNOT_PARSE_ESCAPE_SEQUENCE);
			s += parseEscapeSequence(*buf.position());
			++buf.position();
		}
	}

	throw Exception("Cannot parse quoted string: expected closing quote",
		ErrorCodes::CANNOT_PARSE_QUOTED_STRING);
}


void readQuotedString(String & s, ReadBuffer & buf)
{
	readAnyQuotedString<'\''>(s, buf);
}

void readDoubleQuotedString(String & s, ReadBuffer & buf)
{
	readAnyQuotedString<'"'>(s, buf);
}

void readBackQuotedString(String & s, ReadBuffer & buf)
{
	readAnyQuotedString<'`'>(s, buf);
}


void readException(Exception & e, ReadBuffer & buf, const String & additional_message)
{
	int code = 0;
	String name;
	String message;
	String stack_trace;
	bool has_nested = false;
	
	readBinary(code, buf);
	readBinary(name, buf);
	readBinary(message, buf);
	readBinary(stack_trace, buf);
	readBinary(has_nested, buf);

	std::stringstream message_stream;

	if (!additional_message.empty())
		message_stream << additional_message << ". ";

	if (name != "DB::Exception")
		message_stream << name << ". ";

	message_stream << message
		<< ". Stack trace:\n\n" << stack_trace;

	if (has_nested)
	{
		Exception nested;
		readException(nested, buf);
		e = Exception(message_stream.str(), nested, code);
	}
	else
		e = Exception(message_stream.str(), code);
}

void readAndThrowException(ReadBuffer & buf, const String & additional_message)
{
	Exception e;
	readException(e, buf, additional_message);
	e.rethrow();
}

}
