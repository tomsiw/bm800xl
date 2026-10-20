/*
 * Host test for the boot log's ring buffer and line splitting.
 *
 * The boot log has two pieces of pure logic that are easy to get wrong and
 * impossible to debug on a Pi with a black screen:
 *
 *   1. the ring of lines, which must keep the LAST N lines and wrap correctly;
 *   2. the line splitter used when reading the previous session's log back,
 *      which must handle CRLF, a final line with no newline, and a line longer
 *      than the buffer.
 *
 * This file extracts both in their intended form and checks them against the
 * behaviour bootlog.cpp relies on.  It is compiled for the host, not the Pi.
 *
 * Build and run:
 *   gcc -O1 -Wall -o bootlog_test bootlog_test.c && ./bootlog_test
 */

#include <stdio.h>
#include <string.h>

static int s_nFailures = 0;

static void Check (int bCondition, const char *pWhat)
{
	if (bCondition)
	{
		printf ("  ok   %s\n", pWhat);
	}
	else
	{
		printf ("  FAIL %s\n", pWhat);
		s_nFailures++;
	}
}

/* ---------------------------------------------------------------------------
 * The ring, as bootlog.cpp implements it.
 * ------------------------------------------------------------------------- */

#define MAX_LINE_LENGTH 128
#define MAX_LINES 48
#define MAX_PREVIOUS_LINES 12

typedef struct {
	char Lines[MAX_LINES][MAX_LINE_LENGTH];
	unsigned nCount;
	unsigned nNext;
} Ring;

static void AppendToRing (char (*pRing)[MAX_LINE_LENGTH], unsigned nLines,
			  unsigned *pCount, unsigned *pNext, const char *pLine)
{
	char *pTarget = pRing[*pNext];

	strncpy (pTarget, pLine, MAX_LINE_LENGTH - 1);
	pTarget[MAX_LINE_LENGTH - 1] = '\0';

	*pNext = (*pNext + 1) % nLines;
	if (*pCount < nLines)
	{
		(*pCount)++;
	}
}

/* Returns the i-th line in write order, 0-based. */
static const char *RingGet (Ring *pRing, unsigned i, unsigned nLines)
{
	unsigned nFirst = (pRing->nCount < nLines) ? 0 : pRing->nNext;
	return pRing->Lines[(nFirst + i) % nLines];
}

static void TestRing (void)
{
	printf ("ring buffer\n");

	Ring Ring;
	memset (&Ring, 0, sizeof Ring);

	/* A short run: everything is kept, in order. */
	char Line[64];
	for (int i = 0; i < 5; i++)
	{
		snprintf (Line, sizeof Line, "line %d", i);
		AppendToRing (Ring.Lines, MAX_LINES, &Ring.nCount, &Ring.nNext, Line);
	}
	Check (Ring.nCount == 5, "count reflects 5 appends");
	Check (strcmp (RingGet (&Ring, 0, MAX_LINES), "line 0") == 0,
	       "oldest line first");
	Check (strcmp (RingGet (&Ring, 4, MAX_LINES), "line 4") == 0,
	       "newest line last");

	/* Overflow: the oldest must be dropped, the newest kept. */
	memset (&Ring, 0, sizeof Ring);
	for (int i = 0; i < MAX_LINES + 10; i++)
	{
		snprintf (Line, sizeof Line, "line %d", i);
		AppendToRing (Ring.Lines, MAX_LINES, &Ring.nCount, &Ring.nNext, Line);
	}
	Check (Ring.nCount == MAX_LINES, "count saturates at the ring size");
	Check (strcmp (RingGet (&Ring, 0, MAX_LINES), "line 10") == 0,
	       "after overflow the oldest kept line is the 11th written");
	Check (strcmp (RingGet (&Ring, MAX_LINES - 1, MAX_LINES), "line 57") == 0,
	       "after overflow the newest line is the last written");

	/* A line longer than the buffer is truncated, not wrapped or overrun. */
	memset (&Ring, 0, sizeof Ring);
	char Long[MAX_LINE_LENGTH * 2];
	memset (Long, 'x', sizeof Long - 1);
	Long[sizeof Long - 1] = '\0';
	AppendToRing (Ring.Lines, MAX_LINES, &Ring.nCount, &Ring.nNext, Long);
	Check (strlen (RingGet (&Ring, 0, MAX_LINES)) == MAX_LINE_LENGTH - 1,
	       "an over-long line is truncated to the buffer");
}

/* ---------------------------------------------------------------------------
 * The line splitter, as ReadPreviousLog() implements it.
 * ------------------------------------------------------------------------- */

/*
 * Splits TEXT into lines and prints them, exactly as bootlog.cpp does: strip
 * CR, ignore empty lines, keep the last MAX_PREVIOUS_LINES, and emit a final
 * line with no trailing newline.
 */
static unsigned SplitIntoRing (const char *pText, unsigned nTextLength,
			       char (*pRing)[MAX_LINE_LENGTH], unsigned nLines,
			       unsigned *pCount, unsigned *pNext)
{
	char Line[MAX_LINE_LENGTH];
	unsigned nUsed = 0;

	for (unsigned i = 0; i < nTextLength; i++)
	{
		char Ch = pText[i];

		if (Ch == '\n')
		{
			Line[nUsed] = '\0';
			if (nUsed > 0 && Line[nUsed - 1] == '\r')
			{
				Line[nUsed - 1] = '\0';
			}
			if (Line[0] != '\0')
			{
				AppendToRing (pRing, nLines, pCount, pNext, Line);
			}
			nUsed = 0;
			continue;
		}

		if (nUsed < MAX_LINE_LENGTH - 1)
		{
			Line[nUsed++] = Ch;
		}
	}

	if (nUsed > 0)
	{
		Line[nUsed] = '\0';
		if (nUsed > 0 && Line[nUsed - 1] == '\r')
		{
			Line[nUsed - 1] = '\0';
		}
		if (Line[0] != '\0')
		{
			AppendToRing (pRing, nLines, pCount, pNext, Line);
		}
	}

	return *pCount;
}

static void TestSplitter (void)
{
	printf ("line splitter\n");

	char Ring[MAX_PREVIOUS_LINES][MAX_LINE_LENGTH];
	unsigned nCount, nNext;
	char Out[MAX_LINE_LENGTH];

	/* CRLF, as the writer produces. */
	memset (Ring, 0, sizeof Ring);
	nCount = nNext = 0;
	const char *pCRLF = "alpha\r\nbeta\r\ngamma\r\n";
	SplitIntoRing (pCRLF, strlen (pCRLF), Ring, MAX_PREVIOUS_LINES, &nCount, &nNext);
	Check (nCount == 3, "three CRLF lines produce three entries");
	/* nFirst is 0 here because nothing wrapped. */
	Check (strcmp (Ring[(0 + 0) % MAX_PREVIOUS_LINES], "alpha") == 0,
	       "CR is stripped from the end of a line");
	Check (strcmp (Ring[(0 + 2) % MAX_PREVIOUS_LINES], "gamma") == 0,
	       "the last line is complete");

	/* A final line with no newline must still be emitted. */
	memset (Ring, 0, sizeof Ring);
	nCount = nNext = 0;
	const char *pNoEol = "one\r\ntwo";
	SplitIntoRing (pNoEol, strlen (pNoEol), Ring, MAX_PREVIOUS_LINES, &nCount, &nNext);
	Check (nCount == 2, "a file with no trailing newline keeps its last line");
	Check (strcmp (Ring[(0 + 1) % MAX_PREVIOUS_LINES], "two") == 0,
	       "and that last line is correct");

	/* Empty lines are skipped rather than occupying a slot. */
	memset (Ring, 0, sizeof Ring);
	nCount = nNext = 0;
	const char *pBlanks = "\r\n\r\nreal\r\n\r\n";
	SplitIntoRing (pBlanks, strlen (pBlanks), Ring, MAX_PREVIOUS_LINES, &nCount, &nNext);
	Check (nCount == 1, "blank lines are not stored");
	Check (strcmp (Ring[0], "real") == 0, "the non-blank line is stored");

	/* Overflow keeps the tail, which is the whole point. */
	memset (Ring, 0, sizeof Ring);
	nCount = nNext = 0;
	char Many[4096];
	unsigned nUsed = 0;
	for (int i = 0; i < 30; i++)
	{
		nUsed += (unsigned) snprintf (Many + nUsed, sizeof Many - nUsed,
					      "entry %d\r\n", i);
	}
	SplitIntoRing (Many, nUsed, Ring, MAX_PREVIOUS_LINES, &nCount, &nNext);
	Check (nCount == MAX_PREVIOUS_LINES, "only the ring's worth is kept");

	{
		unsigned nFirst = (nCount < MAX_PREVIOUS_LINES) ? 0 : nNext;
		snprintf (Out, sizeof Out, "%s", Ring[nFirst % MAX_PREVIOUS_LINES]);
		Check (strcmp (Out, "entry 18") == 0,
		       "the oldest kept line is the 19th of 30");
		Check (strcmp (Ring[(nFirst + MAX_PREVIOUS_LINES - 1) % MAX_PREVIOUS_LINES],
			       "entry 29") == 0,
		       "the newest kept line is the last written");
	}
}

int main (void)
{
	printf ("\nboot log logic\n\n");

	TestRing ();
	printf ("\n");
	TestSplitter ();

	printf ("\n%s (%d failure%s)\n\n",
		s_nFailures == 0 ? "PASS" : "FAIL",
		s_nFailures, s_nFailures == 1 ? "" : "s");

	return s_nFailures == 0 ? 0 : 1;
}
