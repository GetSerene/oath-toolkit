/*
 * usersfile.c - implementation of UsersFile based HOTP validation
 * Copyright (C) 2009  Simon Josefsson
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

#include "hotp.h"

#include <stdio.h>		/* For snprintf, getline. */
#include <unistd.h>		/* For ssize_t. */
#include <fcntl.h>		/* For fcntl. */
#include <errno.h>		/* For errno. */

static unsigned
parse_type (const char *str)
{
  if (strcmp (str, "HOTP/E/6") == 0)
    return 6;
  if (strcmp (str, "HOTP/E/7") == 0)
    return 7;
  if (strcmp (str, "HOTP/E/8") == 0)
    return 8;
  if (strcmp (str, "HOTP/E") == 0)
    return 6;
  if (strcmp (str, "HOTP") == 0)
    return 6;
  return 0;
}

static const char *whitespace = " \t\r\n";
static const char *time_format_string = "%Y-%m-%dT%H:%M:%SL";

static int
parse_usersfile (const char *username,
		 const char *otp,
		 size_t window,
		 const char *passwd,
		 time_t * last_otp,
		 FILE * infh,
		 char **lineptr,
		 size_t * n,
		 uint64_t *new_moving_factor)
{
  ssize_t t;

  while ((t = getline (lineptr, n, infh)) != -1)
    {
      char *saveptr;
      char *p = strtok_r (*lineptr, whitespace, &saveptr);
      unsigned digits;
      char secret[20];
      size_t secret_length = sizeof (secret);
      uint64_t start_moving_factor = 0;
      int rc;
      char *prev_otp = NULL;

      if (p == NULL)
	continue;

      /* Read token type */
      digits = parse_type (p);
      if (digits == 0)
	continue;

      /* Read username */
      p = strtok_r (NULL, whitespace, &saveptr);
      if (p == NULL || strcmp (p, username) != 0)
	continue;

      /* Read password. */
      p = strtok_r (NULL, whitespace, &saveptr);
      if (passwd)
	{
	  if (p == NULL)
	    continue;
	  if (strcmp (p, "-") == 0 && *p != '\0')
	    return HOTP_BAD_PASSWORD;
	  if (strcmp (p, passwd) != 0)
	    return HOTP_BAD_PASSWORD;
	}

      /* Read key. */
      p = strtok_r (NULL, whitespace, &saveptr);
      if (p == NULL)
	continue;
      rc = hotp_hex2bin (p, secret, &secret_length);
      if (rc != HOTP_OK)
	return rc;

      /* Read (optional) moving factor. */
      p = strtok_r (NULL, whitespace, &saveptr);
      if (p && *p)
	{
	  char *endptr;
	  unsigned long long int ull = strtoull (p, &endptr, 10);
	  if (endptr && *endptr != '\0')
	    return HOTP_INVALID_COUNTER;
	  start_moving_factor = ull;
	}

      /* Read (optional) last OTP */
      prev_otp = strtok_r (NULL, whitespace, &saveptr);

      /* Read (optional) last_otp */
      p = strtok_r (NULL, whitespace, &saveptr);
      if (p)
	{
	  struct tm tm;
	  char *t;

	  t = strptime (p, time_format_string, &tm);
	  if (t == NULL || *t != '\0')
	    return HOTP_INVALID_TIMESTAMP;
	  tm.tm_isdst = -1;
	  if (last_otp)
	    {
	      *last_otp = mktime (&tm);
	      if (*last_otp == (time_t) - 1)
		return HOTP_INVALID_TIMESTAMP;
	    }
	}

      if (prev_otp && strcmp (prev_otp, otp) == 0)
	return HOTP_REPLAYED_OTP;

      rc = hotp_validate_otp (secret, secret_length,
			      start_moving_factor, window, otp);
      if (rc < 0)
	return rc;
      *new_moving_factor = start_moving_factor + rc;
      return HOTP_OK;
    }

  return HOTP_UNKNOWN_USER;
}

static int
update_usersfile2 (const char *username,
		   const char *otp,
		   FILE * infh,
		   FILE * outfh,
		   char **lineptr,
		   size_t * n,
		   char *timestamp,
		   uint64_t new_moving_factor)
{
  ssize_t t;

  while ((t = getline (lineptr, n, infh)) != -1)
    {
      char *saveptr;
      char *origline;
      char *user, *type, *passwd, *secret;
      int r;

      origline = strdup (*lineptr);

      type = strtok_r (*lineptr, whitespace, &saveptr);
      if (type == NULL)
	continue;

      /* Read username */
      user = strtok_r (NULL, whitespace, &saveptr);
      if (user == NULL || strcmp (user, username) != 0)
	{
	  r = fputs (origline, outfh);
	  if (r <= 0)
	    return HOTP_PRINTF_ERROR;
	  continue;
	}

      passwd = strtok_r (NULL, whitespace, &saveptr);
      if (passwd == NULL)
	passwd = "-";

      secret = strtok_r (NULL, whitespace, &saveptr);
      if (secret == NULL)
	secret = "-";

      r = fprintf (outfh, "%s\t%s\t%s\t%s\t%llu\t%s\t%s\n",
		   type, username, passwd, secret,
		   (unsigned long long) new_moving_factor,
		   otp, timestamp);
      if (r <= 0)
	return HOTP_PRINTF_ERROR;
    }

  return HOTP_OK;
}

static int
update_usersfile (const char *usersfile,
		  const char *username,
		  const char *otp,
		  time_t *last_otp,
		  FILE * infh,
		  char **lineptr,
		  size_t * n,
		  char *timestamp,
		  uint64_t new_moving_factor)
{
  FILE *outfh, *lockfh;
  int rc;
  char *newfilename, *lockfile;

  /* Rewind input file. */
  {
    int pos;

    pos = fseek (infh, 0L, SEEK_SET);
    if (pos == -1)
      return HOTP_FILE_SEEK_ERROR;
    clearerr (infh);
  }

  /* Open lockfile. */
  {
    int l;

    l = asprintf (&lockfile, "%s.lock", usersfile);
    if (lockfile == NULL || l != strlen (usersfile) + 5)
      return HOTP_PRINTF_ERROR;

    lockfh = fopen (lockfile, "w");
    if (!lockfh)
      {
	free (lockfile);
	return HOTP_FILE_CREATE_ERROR;
      }
  }

  /* Lock the lockfile. */
  {
    struct flock l;

    memset (&l, 0, sizeof (l));
    l.l_whence = SEEK_SET;
    l.l_start = 0;
    l.l_len = 0;
    l.l_type = F_WRLCK;

    while ((rc = fcntl (fileno (lockfh), F_SETLKW, &l)) < 0 && errno == EINTR)
      continue;
    if (rc == -1)
      {
	fclose (lockfh);
	free (lockfile);
	return HOTP_FILE_LOCK_ERROR;
      }
  }

  /* Open the "new" file. */
  {
    int l;

    l = asprintf (&newfilename, "%s.new", usersfile);
    if (newfilename == NULL || l != strlen (usersfile) + 4)
      {
	fclose (lockfh);
	free (lockfile);
	return HOTP_PRINTF_ERROR;
      }

    outfh = fopen (newfilename, "w");
    if (!outfh)
      {
	free (newfilename);
	fclose (lockfh);
	free (lockfile);
	return HOTP_FILE_CREATE_ERROR;
      }
  }

  rc = update_usersfile2 (username, otp, infh, outfh, lineptr, n,
			  timestamp, new_moving_factor);

  fclose (lockfh);
  fclose (outfh);

  {
    int tmprc1, tmprc2;

    tmprc1 = rename (newfilename, usersfile);
    free (newfilename);

    tmprc2 = unlink (lockfile);
    free (lockfile);

    if (tmprc1 == -1)
      return HOTP_FILE_RENAME_ERROR;
    if (tmprc2 == -1)
      return HOTP_FILE_UNLINK_ERROR;
  }

  return rc;
}

/**
 * hotp_authenticate_usersfile:
 * @usersfile: string with user credential filename, in UsersFile format
 * @username: string with name of user
 * @otp: string with one-time password to authenticate
 * @window: how many future OTPs to search
 * @passwd: string with password, or %NULL to disable password checking
 * @last_otp: output variable holding last successful authentication
 *
 * Authenticate user named @username with the one-time password @otp
 * and (optional) password @passwd.  Credentials are read (and
 * updated) from a text file named @usersfile.
 *
 * Returns: On successful validation, %HOTP_OK is returned.  If the
 *   supplied @otp is the same as the last successfully authenticated
 *   one-time password, %HOTP_REPLAYED_OTP is returned and the
 *   timestamp of the last authentication is returned in @last_otp.
 *   If the one-time password is not found in the indicated search
 *   window, %HOTP_INVALID_OTP is returned.  Otherwise, an error code
 *   is returned.
 **/
int
hotp_authenticate_usersfile (const char *usersfile,
			     const char *username,
			     const char *otp,
			     size_t window,
			     const char *passwd,
			     time_t * last_otp)
{
  FILE *infh;
  char *line = NULL;
  size_t n = 0;
  uint64_t new_moving_factor;
  int rc;

  infh = fopen (usersfile, "r");
  if (!infh)
    return HOTP_NO_SUCH_FILE;

  rc = parse_usersfile (username, otp, window, passwd, last_otp,
			infh, &line, &n, &new_moving_factor);

  if (rc == HOTP_OK)
    {
      char timestamp[30];
      size_t max = sizeof (timestamp);
      struct tm now;
      time_t t;
      size_t l;
      int r;

      if (time (&t) == (time_t) -1)
	return HOTP_TIME_ERROR;

      if (localtime_r (&t, &now) == NULL)
	return HOTP_TIME_ERROR;

      l = strftime (timestamp, max, time_format_string, &now);
      if (l != 20)
	return HOTP_TIME_ERROR;

      rc = update_usersfile (usersfile, username, otp, last_otp,
			     infh, &line, &n, timestamp,
			     new_moving_factor);
    }

  free (line);
  fclose (infh);

  return rc;
}