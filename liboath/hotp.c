/*
 * hotp.c - implementation of the OATH HOTP algorithm
 * Copyright (C) 2009, 2010, 2011 Simon Josefsson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <config.h>

#include "oath.h"

#include <stdio.h>		/* For snprintf. */

#include "gc.h"

/**
 * oath_hotp_generate:
 * @secret: the shared secret string
 * @secret_length: length of @secret
 * @moving_factor: a counter indicating the current OTP to generate
 * @digits: number of requested digits in the OTP, excluding checksum
 * @add_checksum: whether to add a checksum digit or not
 * @truncation_offset: use a specific truncation offset
 * @output_otp: output buffer, must have room for the output OTP plus zero
 *
 * Generate a one-time-password using the HOTP algorithm as described
 * in RFC 4226.
 *
 * Use a value of %OATH_HOTP_DYNAMIC_TRUNCATION for @truncation_offset
 * unless you really need a specific truncation offset.
 *
 * To find out the size of the OTP you may use the OATH_HOTP_LENGTH()
 * macro.  The @output_otp buffer must be have room for that length
 * plus one for the terminating NUL.
 *
 * Currently only values 6, 7 and 8 for @digits are supported, and the
 * @add_checksum value is ignored.  These restrictions may be lifted
 * in future versions, although some limitations are inherent in the
 * protocol.
 *
 * Returns: On success, %OATH_OK (zero) is returned, otherwise an
 *   error code is returned.
 **/
int
oath_hotp_generate (const char *secret,
		    size_t secret_length,
		    uint64_t moving_factor,
		    unsigned digits,
		    bool add_checksum,
		    size_t truncation_offset, char *output_otp)
{
  char hs[GC_SHA1_DIGEST_SIZE];
  long S;

  {
    char counter[sizeof (moving_factor)];
    size_t i;
    Gc_rc rc;

    for (i = 0; i < sizeof (counter); i++)
      counter[i] =
	(moving_factor >> ((sizeof (moving_factor) - i - 1) * 8)) & 0xFF;

    rc = gc_hmac_sha1 (secret, secret_length,
		       counter, sizeof (moving_factor), hs);
    if (rc != GC_OK)
      return OATH_CRYPTO_ERROR;
  }

  {
    uint8_t offset = hs[sizeof (hs) - 1] & 0x0f;

    S = (((hs[offset] & 0x7f) << 24)
	 | ((hs[offset + 1] & 0xff) << 16)
	 | ((hs[offset + 2] & 0xff) << 8) | ((hs[offset + 3] & 0xff)));

#if DEBUG
    printf ("offset is %d hash is ", offset);
    for (i = 0; i < 20; i++)
      printf ("%02x ", hs[i] & 0xFF);
    printf ("\n");

    printf ("value: %d\n", S);
#endif
  }

  switch (digits)
    {
    case 6:
      S = S % 1000000;
      break;

    case 7:
      S = S % 10000000;
      break;

    case 8:
      S = S % 100000000;
      break;

    default:
      return OATH_INVALID_DIGITS;
      break;
    }

  {
    int len = snprintf (output_otp, digits + 1, "%.*ld", digits, S);
    output_otp[digits] = '\0';
    if (len != digits)
      return OATH_PRINTF_ERROR;
  }

  return OATH_OK;
}

static int
strcmp_callback (void *handle, const char *test_otp)
{
  const char *otp = handle;
  return strcmp (otp, test_otp);
}

/**
 * oath_hotp_validate_callback:
 * @secret: the shared secret string
 * @secret_length: length of @secret
 * @start_moving_factor: start counter in OTP stream
 * @window: how many OTPs after start counter to test
 * @digits: number of requested digits in the OTP
 * @strcmp_otp: function pointer to a strcmp-like function.
 * @strcmp_handle: caller handle to be passed on to @oath_strcmp.
 *
 * Validate an OTP according to OATH HOTP algorithm per RFC 4226.
 *
 * Validation is implemented by generating a number of potential OTPs
 * and performing a call to the @oath_strcmp function, to compare the
 * potential OTP against the given @otp.  It has the following prototype:
 *
 * int (*oath_hotp_validate_strcmp_function) (void *handle, const char *test_otp);
 *
 * The function should behave like strcmp, i.e., only ever return 0 on
 * matches.
 *
 * This callback interface is useful when you cannot compare OTPs
 * directly using normal strcmp, but instead for example only have a
 * hashed OTP.  You would then typically pass in the hashed OTP in the
 * @strcmp_handle and let your implementation of @oath_strcmp hash the
 * test_otp OTP using the same hash, and then compare the results.
 *
 * Currently only OTP lengths of 6, 7 or 8 digits are supported.  This
 * restrictions may be lifted in future versions, although some
 * limitations are inherent in the protocol.
 *
 * Returns: Returns position in OTP window (zero is first position),
 *   or %OATH_INVALID_OTP if no OTP was found in OTP window, or an
 *   error code.
 *
 * Since: 1.4.0
 **/
int
oath_hotp_validate_callback (const char *secret,
			     size_t secret_length,
			     uint64_t start_moving_factor,
			     size_t window,
			     unsigned digits,
			     oath_hotp_validate_strcmp_function oath_strcmp,
			     void *strcmp_handle)
{
  unsigned iter = 0;
  char tmp_otp[10];
  int rc;

  do
    {
      rc = oath_hotp_generate (secret,
			       secret_length,
			       start_moving_factor + iter,
			       digits,
			       false, OATH_HOTP_DYNAMIC_TRUNCATION, tmp_otp);
      if (rc != OATH_OK)
	return rc;

      if (oath_strcmp (strcmp_handle, tmp_otp) == 0)
	return iter;
    }
  while (window - iter++ > 0);

  return OATH_INVALID_OTP;
}

/**
 * oath_hotp_validate:
 * @secret: the shared secret string
 * @secret_length: length of @secret
 * @start_moving_factor: start counter in OTP stream
 * @window: how many OTPs after start counter to test
 * @otp: the OTP to validate.
 *
 * Validate an OTP according to OATH HOTP algorithm per RFC 4226.
 *
 * Currently only OTP lengths of 6, 7 or 8 digits are supported.  This
 * restrictions may be lifted in future versions, although some
 * limitations are inherent in the protocol.
 *
 * Returns: Returns position in OTP window (zero is first position),
 *   or %OATH_INVALID_OTP if no OTP was found in OTP window, or an
 *   error code.
 **/
int
oath_hotp_validate (const char *secret,
		    size_t secret_length,
		    uint64_t start_moving_factor,
		    size_t window, const char *otp)
{
  return oath_hotp_validate_callback (secret, secret_length,
				      start_moving_factor,
				      window, strlen (otp),
				      strcmp_callback, (void *) otp);
}
