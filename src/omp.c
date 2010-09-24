/* OpenVAS Manager
 * $Id$
 * Description: Module for OpenVAS Manager: the OMP library.
 *
 * Authors:
 * Matthew Mundell <matt@mundell.ukfsn.org>
 *
 * Copyright:
 * Copyright (C) 2009 Greenbone Networks GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2,
 * or, at your option, any later version as published by the Free
 * Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * @file  omp.c
 * @brief The OpenVAS Manager OMP library.
 *
 * This file defines an OpenVAS Management Protocol (OMP) library, for
 * implementing OpenVAS managers such as the OpenVAS Manager daemon.
 *
 * The library provides \ref process_omp_client_input.
 * This function parses a given string of OMP XML and tracks and manipulates
 * tasks in reaction to the OMP commands in the string.
 */

/**
 * @internal
 * The OMP-"Processor" is always in a state (\ref client_state_t
 * \ref client_state ) and currently looking at the opening of an OMP element
 * (\ref omp_xml_handle_start_element ), at the text of an OMP element
 * (\ref omp_xml_handle_text ) or at the closing of an OMP element
 * (\ref omp_xml_handle_end_element ).
 *
 * The state usually represents the current location of the parser within the
 * XML (OMP) tree.  There has to be one state for every OMP element.
 *
 * State transitions occur in the start and end element handler callbacks.
 *
 * Generally, the strategy is to wait until the closing of an element before
 * doing any action or sending a response.  Also, error cases are to be detected
 * in the end element handler.
 *
 * If data has to be stored, it goes to \ref command_data (_t) , which is a
 * union.
 * More specific incarnations of this union are e.g. \ref create_user_data (_t)
 * , where the data to create a new user is stored (until the end element of
 * that command is reached).
 *
 * For implementing new commands that have to store data (e.g. not
 * "\<help_extended/\>"), \ref command_data has to be freed and NULL'ed in case
 * of errors and the \ref current_state has to be reset.
 * It can then be assumed that it is NULL'ed at the start of every new
 * command element.  To implement a new start element handler, be sure to just
 * copy an existing case and keep its structure.
 *
 * Attributes are easier to implement than elements.
 * E.g.
 * @code
 * <key_value_pair key="k" value="v"/>
 * @endcode
 * is obviously easier to handle than
 * @code
 * <key><attribute name="k"/><value>v</value></key>
 * @endcode
 * .
 * For this reason the GET commands like GET_TASKS all use attributes only.
 *
 * However, for the other commands it is preferred to avoid attributes and use
 * the text of elements
 * instead, like in
 * @code
 * <key_value_pair><key>k</key><value>v</value></key_value_pair>
 * @endcode
 * .
 *
 * If new elements are built of multiple words, separate the words with an
 * underscore.
 */

#include "omp.h"
#include "manage.h"
/** @todo For access to scanner_t scanner. */
#include "otp.h"
#include "tracef.h"

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <netinet/in.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <openvas/base/certificate.h>
#include <openvas/base/nvti.h>
#include <openvas/base/openvas_string.h>
#include <openvas/nvt_categories.h>
#include <openvas/openvas_logging.h>
#include <openvas/resource_request.h>

#ifdef S_SPLINT_S
#include "splint.h"
#endif

#undef G_LOG_DOMAIN
/**
 * @brief GLib log domain.
 */
#define G_LOG_DOMAIN "md    omp"


/* Static headers. */

/** @todo Exported for manage_sql.c. */
void
buffer_results_xml (GString *, iterator_t *, task_t, int, int, int, int);


/* Helper functions. */

/**
 * @brief Check whether a string is a UUID.
 *
 * @param[in]  uuid  Potential UUID.
 *
 * @return 1 yes, 0 no.
 */
static int
is_uuid (const char *uuid)
{
  while (*uuid) if (isxdigit (*uuid) || (*uuid == '-')) uuid++; else return 0;
  return 1;
}

/**
 * @brief Return the name of a category.
 *
 * @param  category  The number of the category.
 *
 * @return The name of the category.
 */
static const char*
category_name (int category)
{
  static const char *categories[] = { ACT_STRING_LIST_ALL };
  if (category >= ACT_FIRST && category <= ACT_END)
    {
      return categories[category];
    }
  return categories[ACT_UNKNOWN];
}

/** @todo Duplicated from lsc_user.c. */
/**
 * @brief Checks whether a file is a directory or not.
 *
 * This is a replacement for the g_file_test functionality which is reported
 * to be unreliable under certain circumstances, for example if this
 * application and glib are compiled with a different libc.
 *
 * @todo Handle symbolic links.
 * @todo Move to libs?
 *
 * @param[in]  name  File name.
 *
 * @return 1 if parameter is directory, 0 if it is not, -1 if it does not
 *         exist or could not be accessed.
 */
static int
check_is_dir (const char *name)
{
  struct stat sb;

  if (stat (name, &sb))
    {
      return -1;
    }
  else
    {
      return (S_ISDIR (sb.st_mode));
    }
}

/** @todo Duplicated from lsc_user.c. */
/**
 * @brief Recursively removes files and directories.
 *
 * This function will recursively call itself to delete a path and any
 * contents of this path.
 *
 * @todo Exported for manage_sql.c.
 *
 * @param[in]  pathname  Name of file to be deleted from filesystem.
 *
 * @return 0 if the name was successfully deleted, -1 if an error occurred.
 */
int
file_utils_rmdir_rf (const gchar * pathname)
{
  if (check_is_dir (pathname) == 1)
    {
      GError *error = NULL;
      GDir *directory = g_dir_open (pathname, 0, &error);

      if (directory == NULL)
        {
          if (error)
            {
              g_warning ("g_dir_open(%s) failed - %s\n", pathname, error->message);
              g_error_free (error);
            }
          return -1;
        }
      else
        {
          int ret = 0;
          const gchar *entry = NULL;

          while ((entry = g_dir_read_name (directory)) != NULL && (ret == 0))
            {
              gchar *entry_path = g_build_filename (pathname, entry, NULL);
              ret = file_utils_rmdir_rf (entry_path);
              g_free (entry_path);
              if (ret != 0)
                {
                  g_warning ("Failed to remove %s from %s!", entry, pathname);
                  g_dir_close (directory);
                  return ret;
                }
            }
          g_dir_close (directory);
        }
    }

  return g_remove (pathname);
}

/**
 * @brief Return string from ctime with newline replaces with terminator.
 *
 * @param[in]  time  Time.
 *
 * @return Return from ctime applied to time, with newline stripped off.
 */
static char*
ctime_strip_newline (time_t *time)
{
  char* ret = ctime (time);
  if (ret && strlen (ret) > 0)
    ret[strlen (ret) - 1] = '\0';
  return ret;
}

/**
 * @brief Return time defined by broken down time strings.
 *
 * If any argument is NULL, use the value from the current time.
 *
 * @param[in]   hour          Hour (0 to 23).
 * @param[in]   minute        Minute (0 to 59).
 * @param[in]   day_of_month  Day of month (1 to 31).
 * @param[in]   month         Month (1 to 12).
 * @param[in]   year          Year.
 *
 * @return Time described by arguments on success, else -1.
 */
static time_t
time_from_strings (const char *hour, const char *minute,
                   const char *day_of_month, const char *month,
                   const char *year)
{
  struct tm given_broken, *now_broken;
  time_t now;

  time (&now);
  now_broken = localtime (&now);

  given_broken.tm_sec = 0;
  given_broken.tm_min = (minute ? atoi (minute) : now_broken->tm_min);
  given_broken.tm_hour = (hour ? atoi (hour) : now_broken->tm_hour);
  given_broken.tm_mday = (day_of_month
                           ? atoi (day_of_month)
                           : now_broken->tm_mday);
  given_broken.tm_mon = (month ? (atoi (month) - 1) : now_broken->tm_mon);
  given_broken.tm_year = (year ? (atoi (year) - 1900) : now_broken->tm_year);
  given_broken.tm_isdst = now_broken->tm_isdst;

  return mktime (&given_broken);
}

/**
 * @brief Return interval defined by time and unit strings.
 *
 * @param[in]   value   Value.  0 if NULL.
 * @param[in]   unit    Calendar unit: second, minute, hour, day, week,
 *                      month, year or decade.  "second" if NULL.
 * @param[out]  months  Months return.
 *
 * @return Interval described by arguments on success, else -1.
 */
static time_t
interval_from_strings (const char *value, const char *unit, time_t *months)
{
  if (value == NULL)
    return 0;

  if ((unit == NULL) || (strcasecmp (unit, "second") == 0))
    return atoi (value);

  if (strcasecmp (unit, "minute") == 0)
    return atoi (value) * 60;

  if (strcasecmp (unit, "hour") == 0)
    return atoi (value) * 60 * 60;

  if (strcasecmp (unit, "day") == 0)
    return atoi (value) * 60 * 60 * 24;

  if (strcasecmp (unit, "week") == 0)
    return atoi (value) * 60 * 60 * 24 * 7;

  if (months)
    {
      if (strcasecmp (unit, "month") == 0)
        {
          *months = atoi (value);
          return 0;
        }

      if (strcasecmp (unit, "year") == 0)
        {
          *months = atoi (value) * 12;
          return 0;
        }

      if (strcasecmp (unit, "decade") == 0)
        {
          *months = atoi (value) * 12 * 10;
          return 0;
        }
    }

  return -1;
}


/* Help message. */

/**
 * @brief Response to the help command.
 */
static char* help_text = "\n"
"    AUTHENTICATE           Authenticate with the manager.\n"
"    COMMANDS               Run a list of commands.\n"
"    CREATE_AGENT           Create an agent.\n"
"    CREATE_CONFIG          Create a config.\n"
"    CREATE_ESCALATOR       Create an escalator.\n"
"    CREATE_LSC_CREDENTIAL  Create a local security check credential.\n"
"    CREATE_NOTE            Create a note.\n"
"    CREATE_OVERRIDE        Create an override.\n"
"    CREATE_REPORT_FORMAT   Create a report format.\n"
"    CREATE_SCHEDULE        Create a schedule.\n"
"    CREATE_SLAVE           Create a slave.\n"
"    CREATE_TARGET          Create a target.\n"
"    CREATE_TASK            Create a task.\n"
"    DELETE_AGENT           Delete an agent.\n"
"    DELETE_CONFIG          Delete a config.\n"
"    DELETE_ESCALATOR       Delete an escalator.\n"
"    DELETE_LSC_CREDENTIAL  Delete a local security check credential.\n"
"    DELETE_NOTE            Delete a note.\n"
"    DELETE_OVERRIDE        Delete an override.\n"
"    DELETE_REPORT          Delete a report.\n"
"    DELETE_REPORT_FORMAT   Delete a report format.\n"
"    DELETE_SCHEDULE        Delete a schedule.\n"
"    DELETE_SLAVE           Delete a slave.\n"
"    DELETE_TARGET          Delete a target.\n"
"    DELETE_TASK            Delete a task.\n"
"    GET_AGENTS             Get all agents.\n"
#if 0
"    GET_CERTIFICATES       Get all available certificates.\n"
#endif
"    GET_CONFIGS            Get all configs.\n"
"    GET_DEPENDENCIES       Get dependencies for all available NVTs.\n"
"    GET_ESCALATORS         Get all escalators.\n"
"    GET_LSC_CREDENTIALS    Get all local security check credentials.\n"
"    GET_NOTES              Get all notes.\n"
"    GET_NVTS               Get one or all available NVTs.\n"
"    GET_NVT_FAMILIES       Get a list of all NVT families.\n"
"    GET_NVT_FEED_CHECKSUM  Get checksum for entire NVT collection.\n"
"    GET_OVERRIDES          Get all overrides.\n"
"    GET_PREFERENCES        Get preferences for all available NVTs.\n"
"    GET_REPORTS            Get all reports.\n"
"    GET_REPORT_FORMATS     Get all report formats.\n"
"    GET_RESULTS            Get results.\n"
"    GET_SCHEDULES          Get all schedules.\n"
"    GET_SLAVES             Get all slaves.\n"
"    GET_SYSTEM_REPORTS     Get all system reports.\n"
"    GET_TARGET_LOCATORS    Get configured target locators.\n"
"    GET_TARGETS            Get all targets.\n"
"    GET_TASKS              Get all tasks.\n"
"    GET_VERSION            Get the OpenVAS Manager Protocol version.\n"
"    HELP                   Get this help text.\n"
"    MODIFY_CONFIG          Update an existing config.\n"
"    MODIFY_NOTE            Modify an existing note.\n"
"    MODIFY_OVERRIDE        Modify an existing override.\n"
"    MODIFY_REPORT          Modify an existing report.\n"
"    MODIFY_REPORT_FORMAT   Modify an existing report format.\n"
"    MODIFY_TASK            Update an existing task.\n"
"    PAUSE_TASK             Pause a running task.\n"
"    RESUME_OR_START_TASK   Resume task if stopped, else start task.\n"
"    RESUME_PAUSED_TASK     Resume a paused task.\n"
"    RESUME_STOPPED_TASK    Resume a stopped task.\n"
"    START_TASK             Manually start an existing task.\n"
"    STOP_TASK              Stop a running task.\n"
"    TEST_ESCALATOR         Run an escalator.\n"
"    VERIFY_AGENT           Verify an agent.\n"
"    VERIFY_REPORT_FORMAT   Verify a report format.\n";


/* Status codes. */

/* HTTP status codes used:
 *
 *     200 OK
 *     201 Created
 *     202 Accepted
 *     400 Bad request
 *     401 Must auth
 *     404 Missing
 */

/**
 * @brief Response code for a syntax error.
 */
#define STATUS_ERROR_SYNTAX            "400"

/**
 * @brief Response code when authorisation is required.
 */
#define STATUS_ERROR_MUST_AUTH         "401"

/**
 * @brief Response code when authorisation is required.
 */
#define STATUS_ERROR_MUST_AUTH_TEXT    "Authenticate first"

/**
 * @brief Response code for forbidden access.
 */
#define STATUS_ERROR_ACCESS            "403"

/**
 * @brief Response code text for forbidden access.
 */
#define STATUS_ERROR_ACCESS_TEXT       "Access to resource forbidden"

/**
 * @brief Response code for a missing resource.
 */
#define STATUS_ERROR_MISSING           "404"

/**
 * @brief Response code text for a missing resource.
 */
#define STATUS_ERROR_MISSING_TEXT      "Resource missing"

/**
 * @brief Response code for a busy resource.
 */
#define STATUS_ERROR_BUSY              "409"

/**
 * @brief Response code text for a busy resource.
 */
#define STATUS_ERROR_BUSY_TEXT         "Resource busy"

/**
 * @brief Response code when authorisation failed.
 */
#define STATUS_ERROR_AUTH_FAILED       "400"

/**
 * @brief Response code text when authorisation failed.
 */
#define STATUS_ERROR_AUTH_FAILED_TEXT  "Authentication failed"

/**
 * @brief Response code on success.
 */
#define STATUS_OK                      "200"

/**
 * @brief Response code text on success.
 */
#define STATUS_OK_TEXT                 "OK"

/**
 * @brief Response code on success, when a resource is created.
 */
#define STATUS_OK_CREATED              "201"

/**
 * @brief Response code on success, when a resource is created.
 */
#define STATUS_OK_CREATED_TEXT         "OK, resource created"

/**
 * @brief Response code on success, when the operation will finish later.
 */
#define STATUS_OK_REQUESTED            "202"

/**
 * @brief Response code text on success, when the operation will finish later.
 */
#define STATUS_OK_REQUESTED_TEXT       "OK, request submitted"

/**
 * @brief Response code for an internal error.
 */
#define STATUS_INTERNAL_ERROR          "500"

/**
 * @brief Response code text for an internal error.
 */
#define STATUS_INTERNAL_ERROR_TEXT     "Internal error"

/**
 * @brief Response code when a service is down.
 */
#define STATUS_SERVICE_DOWN            "503"

/**
 * @brief Response code text when a service is down.
 */
#define STATUS_SERVICE_DOWN_TEXT       "Service temporarily down"


/* OMP parser. */

/**
 * @brief A handle on an OMP parser.
 */
typedef struct
{
  int (*client_writer) (void*);   ///< Function to write to the client.
  void* client_writer_data;       ///< Argument to client_writer.
} omp_parser_t;

/**
 * @brief Create an OMP parser.
 *
 * @param[in]  write_to_client       Function to write to client.
 * @param[in]  write_to_client_data  Argument to \p write_to_client.
 *
 * @return An OMP parser.
 */
omp_parser_t *
omp_parser_new (int (*write_to_client) (void*), void* write_to_client_data)
{
  omp_parser_t *omp_parser = (omp_parser_t*) g_malloc (sizeof (omp_parser_t));
  omp_parser->client_writer = write_to_client;
  omp_parser->client_writer_data = write_to_client_data;
  return omp_parser;
}

/**
 * @brief Free an OMP parser.
 *
 * @param[in]  omp_parser  OMP parser.
 *
 * @return An OMP parser.
 */
void
omp_parser_free (omp_parser_t *omp_parser)
{
  g_free (omp_parser);
}


/* Command data passed between parser callbacks. */

/**
 * @brief Create a new preference.
 *
 * @param[in]  name      Name of preference.
 * @param[in]  type      Type of preference.
 * @param[in]  value     Value of preference.
 * @param[in]  nvt_name  Name of NVT of preference.
 * @param[in]  nvt_oid   OID of NVT of preference.
 * @param[in]  alts      Array of gchar's.  Alternative values for type radio.
 *
 * @return Newly allocated preference.
 */
static gpointer
preference_new (char *name, char *type, char *value, char *nvt_name,
                char *nvt_oid, array_t *alts)
{
  preference_t *preference;

  preference = (preference_t*) g_malloc0 (sizeof (preference_t));
  preference->name = name;
  preference->type = type;
  preference->value = value;
  preference->nvt_name = nvt_name;
  preference->nvt_oid = nvt_oid;
  preference->alts = alts;

  return preference;
}

/**
 * @brief Create a new NVT selector.
 *
 * @param[in]  name           Name of NVT selector.
 * @param[in]  type           Type of NVT selector.
 * @param[in]  include        Include/exclude flag.
 * @param[in]  family_or_nvt  Family or NVT.
 *
 * @return Newly allocated NVT selector.
 */
static gpointer
nvt_selector_new (char *name, char *type, int include, char *family_or_nvt)
{
  nvt_selector_t *selector;

  selector = (nvt_selector_t*) g_malloc0 (sizeof (nvt_selector_t));
  selector->name = name;
  selector->type = type;
  selector->include = include;
  selector->family_or_nvt = family_or_nvt;

  return selector;
}

/**
 * @brief Command data for the create_agent command.
 */
typedef struct
{
  char *comment;                  ///< Comment.
  char *howto_install;            ///< Install HOWTO.
  char *howto_use;                ///< Usage HOWTO.
  char *installer;                ///< Installer content.
  char *installer_filename;       ///< Installer filename.
  char *installer_signature;      ///< Installer signature.
  char *name;                     ///< Agent name.
} create_agent_data_t;

/**
 * @brief Free members of a create_agent_data_t and set them to NULL.
 */
static void
create_agent_data_reset (create_agent_data_t *data)
{
  free (data->comment);
  free (data->howto_install);
  free (data->howto_use);
  free (data->installer);
  free (data->installer_filename);
  free (data->installer_signature);
  free (data->name);

  memset (data, 0, sizeof (create_agent_data_t));
}

/**
 * @brief Command data for the import part of the create_config command.
 */
typedef struct
{
  int import;                        ///< The import element was present.
  char *comment;                     ///< Comment.
  char *name;                        ///< Config name.
  array_t *nvt_selectors;            ///< Array of nvt_selector_t's.
  char *nvt_selector_name;           ///< In NVT_SELECTORS name of selector.
  char *nvt_selector_type;           ///< In NVT_SELECTORS type of selector.
  char *nvt_selector_include;        ///< In NVT_SELECTORS include/exclude flag.
  char *nvt_selector_family_or_nvt;  ///< In NVT_SELECTORS family/NVT flag.
  array_t *preferences;              ///< Array of preference_t's.
  array_t *preference_alts;          ///< Array of gchar's in PREFERENCES.
  char *preference_alt;              ///< Single radio alternative in PREFERENCE.
  char *preference_name;             ///< Name in PREFERENCE.
  char *preference_nvt_name;         ///< NVT name in PREFERENCE.
  char *preference_nvt_oid;          ///< NVT OID in PREFERENCE.
  char *preference_type;             ///< Type in PREFERENCE.
  char *preference_value;            ///< Value in PREFERENCE.
} import_config_data_t;

/**
 * @brief Command data for the create_config command.
 */
typedef struct
{
  char *comment;                     ///< Comment.
  char *copy;                        ///< Config to copy.
  import_config_data_t import;       ///< Config to import.
  char *name;                        ///< Name.
  char *rcfile;                      ///< RC file from which to create config.
} create_config_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_config_data_reset (create_config_data_t *data)
{
  int index = 0;
  const preference_t *preference;
  import_config_data_t *import = (import_config_data_t*) &data->import;

  free (data->comment);
  free (data->copy);

  free (import->comment);
  free (import->name);
  array_free (import->nvt_selectors);
  free (import->nvt_selector_name);
  free (import->nvt_selector_type);
  free (import->nvt_selector_family_or_nvt);

  if (import->preferences)
    {
      while ((preference = (preference_t*) g_ptr_array_index (import->preferences,
                                                              index++)))
        array_free (preference->alts);
      array_free (import->preferences);
    }

  free (import->preference_alt);
  free (import->preference_name);
  free (import->preference_nvt_name);
  free (import->preference_nvt_oid);
  free (import->preference_type);
  free (import->preference_value);

  free (data->name);
  free (data->rcfile);

  memset (data, 0, sizeof (create_config_data_t));
}

/**
 * @brief Command data for the create_escalator command.
 *
 * The pointers in the *_data arrays point to memory that contains two
 * strings concatentated, with a single \\0 between them.  The first string
 * is the name of the extra data (for example "To Address"), the second is
 * the value the the data (for example "alice@example.org").
 */
typedef struct
{
  char *comment;             ///< Comment.
  char *condition;           ///< Condition for escalation, e.g. "Always".
  array_t *condition_data;   ///< Array of pointers.  Extra data for condition.
  char *event;               ///< Event that will cause escalation.
  array_t *event_data;       ///< Array of pointers.  Extra data for event.
  char *method;              ///< Method of escalation, e.g. "Email".
  array_t *method_data;      ///< Array of pointer.  Extra data for method.
  char *name;                ///< Name of escalator.
  char *part_data;           ///< Second part of data during *_data: value.
  char *part_name;           ///< First part of data during *_data: name.
} create_escalator_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_escalator_data_reset (create_escalator_data_t *data)
{
  free (data->comment);
  free (data->condition);
  array_free (data->condition_data);
  free (data->event);
  array_free (data->event_data);
  free (data->method);
  array_free (data->method_data);
  free (data->name);
  free (data->part_data);
  free (data->part_name);

  memset (data, 0, sizeof (create_escalator_data_t));
}

/**
 * @brief Command data for the create_lsc_credential command.
 */
typedef struct
{
  char *comment;           ///< Comment.
  char *login;             ///< Login name.
  char *name;              ///< LSC credential name.
  char *password;          ///< Password associated with login name.
} create_lsc_credential_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_lsc_credential_data_reset (create_lsc_credential_data_t *data)
{
  free (data->comment);
  free (data->login);
  free (data->name);
  free (data->password);

  memset (data, 0, sizeof (create_lsc_credential_data_t));
}

/**
 * @brief Command data for the create_note command.
 */
typedef struct
{
  char *hosts;        ///< Hosts to which to limit override.
  char *nvt_oid;      ///< NVT to which to limit override.
  char *port;         ///< Port to which to limit override.
  char *result_id;    ///< ID of result to which to limit override.
  char *task_id;      ///< ID of task to which to limit override.
  char *text;         ///< Text of override.
  char *threat;       ///< Threat to which to limit override.
} create_note_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_note_data_reset (create_note_data_t *data)
{
  free (data->hosts);
  free (data->nvt_oid);
  free (data->port);
  free (data->result_id);
  free (data->task_id);
  free (data->text);
  free (data->threat);

  memset (data, 0, sizeof (create_note_data_t));
}

/**
 * @brief Command data for the create_override command.
 */
typedef struct
{
  char *hosts;        ///< Hosts to which to limit override.
  char *new_threat;   ///< New threat value of overridden results.
  char *nvt_oid;      ///< NVT to which to limit override.
  char *port;         ///< Port to which to limit override.
  char *result_id;    ///< ID of result to which to limit override.
  char *task_id;      ///< ID of task to which to limit override.
  char *text;         ///< Text of override.
  char *threat;       ///< Threat to which to limit override.
} create_override_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_override_data_reset (create_override_data_t *data)
{
  free (data->hosts);
  free (data->new_threat);
  free (data->nvt_oid);
  free (data->port);
  free (data->result_id);
  free (data->task_id);
  free (data->text);
  free (data->threat);

  memset (data, 0, sizeof (create_override_data_t));
}

/**
 * @brief Command data for the create_report_format command.
 */
typedef struct
{
  char *content_type;     ///< Content type.
  char *description;      ///< Description.
  char *extension;        ///< File extension.
  char *file;             ///< Current file during ...GRFR_REPORT_FORMAT_FILE.
  char *file_name;        ///< Name of current file.
  array_t *files;         ///< All files.
  char *global;           ///< Global flag.
  char *id;               ///< ID.
  int import;             ///< Boolean.  Whether to import a format.
  char *name;             ///< Name.
  char *param_value;      ///< Param value during ...GRFR_REPORT_FORMAT_PARAM.
  char *param_name;       ///< Name of above param.
  array_t *params;        ///< All params.
  char *signature;        ///< Signature.
  char *summary;          ///< Summary.
} create_report_format_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_report_format_data_reset (create_report_format_data_t *data)
{
  free (data->content_type);
  free (data->description);
  free (data->extension);
  free (data->file);
  free (data->file_name);
  array_free (data->files);
  free (data->global);
  free (data->id);
  free (data->name);
  free (data->param_name);
  free (data->param_value);
  array_free (data->params);
  free (data->summary);

  memset (data, 0, sizeof (create_report_format_data_t));
}

/**
 * @brief Command data for the create_schedule command.
 */
typedef struct
{
  char *name;                    ///< Name for new schedule.
  char *comment;                 ///< Comment.
  char *first_time_day_of_month; ///< Day of month schedule must first run.
  char *first_time_hour;         ///< Hour schedule must first run.
  char *first_time_minute;       ///< Minute schedule must first run.
  char *first_time_month;        ///< Month schedule must first run.
  char *first_time_year;         ///< Year schedule must first run.
  char *period;                  ///< Period of schedule (how often it runs).
  char *period_unit;             ///< Unit of period: "hour", "day", "week", ....
  char *duration;                ///< Duration of schedule (how long it runs for).
  char *duration_unit;           ///< Unit of duration: "hour", "day", "week", ....
} create_schedule_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_schedule_data_reset (create_schedule_data_t *data)
{
  free (data->name);
  free (data->comment);
  free (data->first_time_day_of_month);
  free (data->first_time_hour);
  free (data->first_time_minute);
  free (data->first_time_month);
  free (data->first_time_year);
  free (data->period);
  free (data->period_unit);
  free (data->duration);
  free (data->duration_unit);

  memset (data, 0, sizeof (create_schedule_data_t));
}

/**
 * @brief Command data for the create_slave command.
 */
typedef struct
{
  char *comment;                 ///< Comment.
  char *host;                    ///< Host for new slave.
  char *login;                   ///< Login on slave.
  char *name;                    ///< Name of new slave.
  char *password;                ///< Password for login.
  char *port;                    ///< Port on host.
} create_slave_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_slave_data_reset (create_slave_data_t *data)
{
  free (data->comment);
  free (data->host);
  free (data->login);
  free (data->name);
  free (data->password);
  free (data->port);

  memset (data, 0, sizeof (create_slave_data_t));
}

/**
 * @brief Command data for the create_target command.
 */
typedef struct
{
  char *comment;                 ///< Comment.
  char *hosts;                   ///< Hosts for new target.
  char *lsc_credential_id;       ///< LSC credential for new target.
  char *name;                    ///< Name of new target.
  char *target_locator;          ///< Target locator (source name).
  char *target_locator_password; ///< Target locator credentials: password.
  char *target_locator_username; ///< Target locator credentials: username.
} create_target_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_target_data_reset (create_target_data_t *data)
{
  free (data->comment);
  free (data->hosts);
  free (data->lsc_credential_id);
  free (data->name);
  free (data->target_locator);
  free (data->target_locator_password);
  free (data->target_locator_username);

  memset (data, 0, sizeof (create_target_data_t));
}

/**
 * @brief Command data for the create_task command.
 */
typedef struct
{
  char *config_id;      ///< ID of task config.
  char *escalator_id;   ///< ID of task escalator.
  char *schedule_id;    ///< ID of task schedule.
  char *slave_id;       ///< ID of task slave.
  char *target_id;      ///< ID of task target.
  task_t task;          ///< ID of new task.
} create_task_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_task_data_reset (create_task_data_t *data)
{
  free (data->config_id);
  free (data->escalator_id);
  free (data->schedule_id);
  free (data->slave_id);
  free (data->target_id);

  memset (data, 0, sizeof (create_task_data_t));
}

/**
 * @brief Command data for the delete_agent command.
 */
typedef struct
{
  char *agent_id;   ///< ID of agent to delete.
} delete_agent_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_agent_data_reset (delete_agent_data_t *data)
{
  free (data->agent_id);

  memset (data, 0, sizeof (delete_agent_data_t));
}

/**
 * @brief Command data for the delete_config command.
 */
typedef struct
{
  char *config_id;   ///< ID of config to delete.
} delete_config_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_config_data_reset (delete_config_data_t *data)
{
  free (data->config_id);

  memset (data, 0, sizeof (delete_config_data_t));
}

/**
 * @brief Command data for the delete_escalator command.
 */
typedef struct
{
  char *escalator_id;   ///< ID of escalator to delete.
} delete_escalator_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_escalator_data_reset (delete_escalator_data_t *data)
{
  free (data->escalator_id);

  memset (data, 0, sizeof (delete_escalator_data_t));
}

/**
 * @brief Command data for the delete_lsc_credential command.
 */
typedef struct
{
  char *lsc_credential_id;   ///< ID of LSC credential to delete.
} delete_lsc_credential_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_lsc_credential_data_reset (delete_lsc_credential_data_t *data)
{
  free (data->lsc_credential_id);

  memset (data, 0, sizeof (delete_lsc_credential_data_t));
}

/**
 * @brief Command data for the delete_note command.
 */
typedef struct
{
  char *note_id;   ///< ID of note to delete.
} delete_note_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_note_data_reset (delete_note_data_t *data)
{
  free (data->note_id);

  memset (data, 0, sizeof (delete_note_data_t));
}

/**
 * @brief Command data for the delete_override command.
 */
typedef struct
{
  char *override_id;   ///< ID of override to delete.
} delete_override_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_override_data_reset (delete_override_data_t *data)
{
  free (data->override_id);

  memset (data, 0, sizeof (delete_override_data_t));
}

/**
 * @brief Command data for the delete_report command.
 */
typedef struct
{
  char *report_id;   ///< ID of report to delete.
} delete_report_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_report_data_reset (delete_report_data_t *data)
{
  free (data->report_id);

  memset (data, 0, sizeof (delete_report_data_t));
}

/**
 * @brief Command data for the delete_report_format command.
 */
typedef struct
{
  char *report_format_id;   ///< ID of report format to delete.
} delete_report_format_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_report_format_data_reset (delete_report_format_data_t *data)
{
  free (data->report_format_id);

  memset (data, 0, sizeof (delete_report_format_data_t));
}

/**
 * @brief Command data for the delete_schedule command.
 */
typedef struct
{
  char *schedule_id;   ///< ID of schedule to delete.
} delete_schedule_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_schedule_data_reset (delete_schedule_data_t *data)
{
  free (data->schedule_id);

  memset (data, 0, sizeof (delete_schedule_data_t));
}

/**
 * @brief Command data for the delete_slave command.
 */
typedef struct
{
  char *slave_id;   ///< ID of slave to delete.
} delete_slave_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_slave_data_reset (delete_slave_data_t *data)
{
  free (data->slave_id);

  memset (data, 0, sizeof (delete_slave_data_t));
}

/**
 * @brief Command data for the delete_target command.
 */
typedef struct
{
  char *target_id;   ///< ID of target to delete.
} delete_target_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_target_data_reset (delete_target_data_t *data)
{
  free (data->target_id);

  memset (data, 0, sizeof (delete_target_data_t));
}

/**
 * @brief Command data for the delete_task command.
 */
typedef struct
{
  char *task_id;   ///< ID of task to delete.
} delete_task_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_task_data_reset (delete_task_data_t *data)
{
  free (data->task_id);

  memset (data, 0, sizeof (delete_task_data_t));
}

/**
 * @brief Command data for the get_agents command.
 */
typedef struct
{
  char *agent_id;        ///< ID of single agent to get.
  char *format;          ///< Format requested: "installer", "howto_use", ....
  char *sort_field;      ///< Field to sort results on.
  int sort_order;        ///< Result sort order: 0 descending, else ascending.
} get_agents_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_agents_data_reset (get_agents_data_t *data)
{
  free (data->agent_id);
  free (data->format);
  free (data->sort_field);

  memset (data, 0, sizeof (get_agents_data_t));
}

/**
 * @brief Command data for the get_configs command.
 */
typedef struct
{
  int export;            ///< Boolean.  Whether to format for create_config.
  int families;          ///< Boolean.  Whether to include config families.
  char *config_id;       ///< ID of single config to iterate over.
  int preferences;       ///< Boolean.  Whether to include config preferences.
  char *sort_field;      ///< Field to sort results on.
  int sort_order;        ///< Result sort order: 0 descending, else ascending.
} get_configs_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_configs_data_reset (get_configs_data_t *data)
{
  free (data->config_id);
  free (data->sort_field);

  memset (data, 0, sizeof (get_configs_data_t));
}

/**
 * @brief Command data for the get_dependencies command.
 */
typedef struct
{
  char *nvt_oid;  ///< OID of single NVT whose  dependencies to get.
} get_dependencies_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_dependencies_data_reset (get_dependencies_data_t *data)
{
  free (data->nvt_oid);

  memset (data, 0, sizeof (get_dependencies_data_t));
}

/**
 * @brief Command data for the get_escalators command.
 */
typedef struct
{
  char *escalator_id;    ///< ID of single escalator to get.
  char *sort_field;      ///< Field to sort results on.
  int sort_order;        ///< Result sort order: 0 descending, else ascending.
} get_escalators_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_escalators_data_reset (get_escalators_data_t *data)
{
  free (data->escalator_id);
  free (data->sort_field);

  memset (data, 0, sizeof (get_escalators_data_t));
}

/**
 * @brief Command data for the get_lsc_credentials command.
 */
typedef struct
{
  char *format;            ///< Format requested: "key", "deb", ....
  char *lsc_credential_id; ///< Single LSC credential to iterate over.
  char *sort_field;        ///< Field to sort results on.
  int sort_order;          ///< Result sort order: 0 descending, else ascending.
} get_lsc_credentials_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_lsc_credentials_data_reset (get_lsc_credentials_data_t *data)
{
  free (data->format);
  free (data->lsc_credential_id);
  free (data->sort_field);

  memset (data, 0, sizeof (get_lsc_credentials_data_t));
}

/**
 * @brief Command data for the get_notes command.
 */
typedef struct
{
  char *note_id;         ///< ID of single note to get.
  char *nvt_oid;         ///< OID of NVT to which to limit listing.
  char *task_id;         ///< ID of task to which to limit listing.
  char *sort_field;      ///< Field to sort results on.
  int sort_order;        ///< Result sort order: 0 descending, else ascending.
  int details;           ///< Boolean.  Whether to include full note details.
  int result;            ///< Boolean.  Whether to include associated results.
} get_notes_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_notes_data_reset (get_notes_data_t *data)
{
  free (data->note_id);
  free (data->nvt_oid);
  free (data->task_id);

  memset (data, 0, sizeof (get_notes_data_t));
}

/**
 * @brief Command data for the get_nvts command.
 */
typedef struct
{
  char *config_id;       ///< ID of config to which to limit NVT selection.
  int details;           ///< Boolean.  Whether to include full NVT details.
  char *family;          ///< Name of family to which to limit NVT selection.
  char *nvt_oid;         ///< Name of single NVT to get.
  int preference_count;  ///< Boolean.  Whether to include NVT preference count.
  int preferences;       ///< Boolean.  Whether to include NVT preferences.
  char *sort_field;      ///< Field to sort results on.
  int sort_order;        ///< Result sort order: 0 descending, else ascending.
  int timeout;           ///< Boolean.  Whether to include timeout preference.
} get_nvts_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_nvts_data_reset (get_nvts_data_t *data)
{
  free (data->config_id);
  free (data->family);
  free (data->nvt_oid);
  free (data->sort_field);

  memset (data, 0, sizeof (get_nvts_data_t));
}

/**
 * @brief Command data for the get_nvt_families command.
 */
typedef struct
{
  int sort_order;        ///< Result sort order: 0 descending, else ascending.
} get_nvt_families_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_nvt_families_data_reset (get_nvt_families_data_t *data)
{
  memset (data, 0, sizeof (get_nvt_families_data_t));
}

/**
 * @brief Command data for the get_nvt_feed_checksum command.
 */
typedef struct
{
  char *algorithm;  ///< Algorithm requested by client.
} get_nvt_feed_checksum_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_nvt_feed_checksum_data_reset (get_nvt_feed_checksum_data_t *data)
{
  free (data->algorithm);

  memset (data, 0, sizeof (get_nvt_feed_checksum_data_t));
}

/**
 * @brief Command data for the get_overrides command.
 */
typedef struct
{
  char *override_id;   ///< ID of override to get.
  char *nvt_oid;       ///< OID of NVT to which to limit listing.
  char *task_id;       ///< ID of task to which to limit listing.
  char *sort_field;    ///< Field to sort results on.
  int sort_order;      ///< Result sort order: 0 descending, else ascending.
  int details;         ///< Boolean.  Whether to include full override details.
  int result;          ///< Boolean.  Whether to include associated results.
} get_overrides_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_overrides_data_reset (get_overrides_data_t *data)
{
  free (data->override_id);
  free (data->nvt_oid);
  free (data->task_id);

  memset (data, 0, sizeof (get_overrides_data_t));
}

/**
 * @brief Command data for the get_preferences command.
 */
typedef struct
{
  char *config_id;  ///< Config whose preference values to get.
  char *nvt_oid;    ///< Single NVT whose preferences to get.
  char *preference; ///< Single preference to get.
} get_preferences_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_preferences_data_reset (get_preferences_data_t *data)
{
  free (data->config_id);
  free (data->nvt_oid);
  free (data->preference);

  memset (data, 0, sizeof (get_preferences_data_t));
}

/**
 * @brief Command data for the get_reports command.
 */
typedef struct
{
  int apply_overrides;   ///< Boolean.  Whether to apply overrides to results.
  char *format_id;       ///< ID of report format.
  char *report_id;       ///< ID of single report to get.
  int first_result;      ///< Skip over results before this result number.
  int max_results;       ///< Maximum number of results return.
  char *sort_field;      ///< Field to sort results on.
  int sort_order;        ///< Result sort order: 0 descending, else ascending.
  char *levels;          ///< Letter encoded threat level filter.
  char *search_phrase;   ///< Search phrase result filter.
  char *min_cvss_base;   ///< Minimum CVSS base filter.
  int notes;             ///< Boolean.  Whether to include associated notes.
  int notes_details;     ///< Boolean.  Whether to include details of above.
  int overrides;         ///< Boolean.  Whether to include associated overrides.
  int overrides_details; ///< Boolean.  Whether to include details of above.
  int result_hosts_only; ///< Boolean.  Whether to include only resulted hosts.
} get_reports_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_reports_data_reset (get_reports_data_t *data)
{
  free (data->format_id);
  free (data->report_id);
  free (data->sort_field);
  free (data->levels);
  free (data->search_phrase);
  free (data->min_cvss_base);

  memset (data, 0, sizeof (get_reports_data_t));
}

/**
 * @brief Command data for the get_report_formats command.
 */
typedef struct
{
  int export;            ///< Boolean.  Whether to format for importing.
  int params;            ///< Boolean.  Whether to include params.
  char *sort_field;       ///< Field to sort results on.
  int sort_order;         ///< Result sort order: 0 descending, else ascending.
  char *report_format_id; ///< ID of single report format to get.
} get_report_formats_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_report_formats_data_reset (get_report_formats_data_t *data)
{
  free (data->report_format_id);
  free (data->sort_field);

  memset (data, 0, sizeof (get_report_formats_data_t));
}

/**
 * @brief Command data for the get_results command.
 */
typedef struct
{
  int apply_overrides;   ///< Boolean.  Whether to apply overrides to results.
  char *result_id;       ///< ID of single result to get.
  char *task_id;         ///< Task associated with results.
  int notes;             ///< Boolean.  Whether to include associated notes.
  int notes_details;     ///< Boolean.  Whether to include details of above.
  int overrides;         ///< Boolean.  Whether to include associated overrides.
  int overrides_details; ///< Boolean.  Whether to include details of above.
} get_results_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_results_data_reset (get_results_data_t *data)
{
  free (data->result_id);
  free (data->task_id);

  memset (data, 0, sizeof (get_results_data_t));
}

/**
 * @brief Command data for the get_schedules command.
 */
typedef struct
{
  char *schedule_id;     ///< ID of single schedule to get.
  char *sort_field;      ///< Field to sort results on.
  int sort_order;        ///< Result sort order: 0 descending, else ascending.
  int details;           ///< Boolean.  Whether to include full details.
} get_schedules_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_schedules_data_reset (get_schedules_data_t *data)
{
  free (data->schedule_id);

  memset (data, 0, sizeof (get_schedules_data_t));
}

/**
 * @brief Command data for the get_slaves command.
 */
typedef struct
{
  char *sort_field;    ///< Field to sort results on.
  int sort_order;      ///< Result sort order: 0 descending, else ascending.
  char *slave_id;      ///< ID of single slave to get.
  int tasks;           ///< Boolean.  Whether to include tasks that use slave.
} get_slaves_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_slaves_data_reset (get_slaves_data_t *data)
{
  free (data->slave_id);
  free (data->sort_field);

  memset (data, 0, sizeof (get_slaves_data_t));
}

/**
 * @brief Command data for the get_system_reports command.
 */
typedef struct
{
  int brief;        ///< Boolean.  Whether respond in brief.
  char *name;       ///< Name of single report to get.
  char *duration;   ///< Duration into the past to report on.
} get_system_reports_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_system_reports_data_reset (get_system_reports_data_t *data)
{
  free (data->name);
  free (data->duration);

  memset (data, 0, sizeof (get_system_reports_data_t));
}

/**
 * @brief Command data for the get_targets command.
 */
typedef struct
{
  char *sort_field;    ///< Field to sort results on.
  int sort_order;      ///< Result sort order: 0 descending, else ascending.
  char *target_id;     ///< ID of single target to get.
  int tasks;           ///< Boolean.  Whether to include tasks that use target.
} get_targets_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_targets_data_reset (get_targets_data_t *data)
{
  free (data->target_id);
  free (data->sort_field);

  memset (data, 0, sizeof (get_targets_data_t));
}

/**
 * @brief Command data for the modify_config command.
 */
typedef struct
{
  char *config_id;                     ///< ID of config to modify.
  array_t *families_growing_empty; ///< New family selection: growing, empty.
  array_t *families_growing_all;   ///< New family selection: growing, all NVTs.
  array_t *families_static_all;    ///< New family selection: static, all NVTs.
  int family_selection_family_all;     ///< All flag in FAMILY_SELECTION/FAMILY.
  char *family_selection_family_all_text; ///< Text version of above.
  int family_selection_family_growing; ///< FAMILY_SELECTION/FAMILY growing flag.
  char *family_selection_family_growing_text; ///< Text version of above.
  char *family_selection_family_name;  ///< FAMILY_SELECTION/FAMILY family name.
  int family_selection_growing;        ///< Whether families in selection grow.
  char *family_selection_growing_text; ///< Text version of above.
  array_t *nvt_selection;              ///< OID array. New NVT set for config.
  char *nvt_selection_family;          ///< Family of NVT selection.
  char *nvt_selection_nvt_oid;         ///< OID during NVT_selection/NVT.
  char *preference_name;               ///< Config preference to modify.
  char *preference_nvt_oid;            ///< OID of NVT of preference.
  char *preference_value;              ///< New value for preference.
} modify_config_data_t;

/**
 * @brief Command data for the get_tasks command.
 */
typedef struct
{
  int apply_overrides;   ///< Boolean.  Whether to apply overrides.
  int details;           ///< Boolean.  Whether to include task details.
  char *task_id;         ///< ID of single task to get.
  int rcfile;            ///< Boolean.  Whether to include RC defining task.
  char *sort_field;      ///< Field to sort results on.
  int sort_order;        ///< Result sort order: 0 descending, else ascending.
} get_tasks_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_tasks_data_reset (get_tasks_data_t *data)
{
  free (data->task_id);
  free (data->sort_field);

  memset (data, 0, sizeof (get_tasks_data_t));
}

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_config_data_reset (modify_config_data_t *data)
{
  free (data->config_id);
  array_free (data->families_growing_empty);
  array_free (data->families_growing_all);
  array_free (data->families_static_all);
  free (data->family_selection_family_all_text);
  free (data->family_selection_family_growing_text);
  free (data->family_selection_family_name);
  free (data->family_selection_growing_text);
  array_free (data->nvt_selection);
  free (data->nvt_selection_family);
  free (data->nvt_selection_nvt_oid);
  free (data->preference_name);
  free (data->preference_nvt_oid);
  free (data->preference_value);

  memset (data, 0, sizeof (modify_config_data_t));
}

/**
 * @brief Command data for the modify_report command.
 */
typedef struct
{
  char *comment;       ///< Comment.
  char *report_id;     ///< ID of report to modify.
} modify_report_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_report_data_reset (modify_report_data_t *data)
{
  free (data->comment);
  free (data->report_id);

  memset (data, 0, sizeof (modify_report_data_t));
}

/**
 * @brief Command data for the modify_report_format command.
 */
typedef struct
{
  char *name;                 ///< Name.
  char *report_format_id;     ///< ID of report format to modify.
  char *summary;              ///< Summary.
} modify_report_format_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_report_format_data_reset (modify_report_format_data_t *data)
{
  free (data->name);
  free (data->report_format_id);
  free (data->summary);

  memset (data, 0, sizeof (modify_report_format_data_t));
}

/**
 * @brief Command data for the modify_task command.
 */
typedef struct
{
  char *action;        ///< What to do to file: "update" or "remove".
  char *comment;       ///< Comment.
  char *escalator_id;  ///< ID of new escalator for task.
  char *file;          ///< File to attach to task.
  char *file_name;     ///< Name of file to attach to task.
  char *name;          ///< New name for task.
  char *rcfile;        ///< New definition for task, as an RC file.
  char *schedule_id;   ///< ID of new schedule for task.
  char *task_id;       ///< ID of task to modify.
} modify_task_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_task_data_reset (modify_task_data_t *data)
{
  free (data->action);
  free (data->comment);
  free (data->escalator_id);
  free (data->file);
  free (data->file_name);
  free (data->name);
  free (data->rcfile);
  free (data->schedule_id);
  free (data->task_id);

  memset (data, 0, sizeof (modify_task_data_t));
}

/**
 * @brief Command data for the modify_note command.
 */
typedef struct
{
  char *hosts;        ///< Hosts to which to limit override.
  char *note_id;      ///< ID of note to modify.
  char *nvt_oid;      ///< NVT to which to limit override.
  char *port;         ///< Port to which to limit override.
  char *result_id;    ///< ID of result to which to limit override.
  char *task_id;      ///< ID of task to which to limit override.
  char *text;         ///< Text of override.
  char *threat;       ///< Threat to which to limit override.
} modify_note_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_note_data_reset (modify_note_data_t *data)
{
  free (data->hosts);
  free (data->note_id);
  free (data->nvt_oid);
  free (data->port);
  free (data->result_id);
  free (data->task_id);
  free (data->text);
  free (data->threat);

  memset (data, 0, sizeof (modify_note_data_t));
}

/**
 * @brief Command data for the modify_override command.
 */
typedef struct
{
  char *hosts;        ///< Hosts to which to limit override.
  char *new_threat;   ///< New threat value of overridden results.
  char *nvt_oid;      ///< NVT to which to limit override.
  char *override_id;  ///< ID of override to modify.
  char *port;         ///< Port to which to limit override.
  char *result_id;    ///< ID of result to which to limit override.
  char *task_id;      ///< ID of task to which to limit override.
  char *text;         ///< Text of override.
  char *threat;       ///< Threat to which to limit override.
} modify_override_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_override_data_reset (modify_override_data_t *data)
{
  free (data->hosts);
  free (data->new_threat);
  free (data->nvt_oid);
  free (data->override_id);
  free (data->port);
  free (data->result_id);
  free (data->task_id);
  free (data->text);
  free (data->threat);

  memset (data, 0, sizeof (modify_override_data_t));
}

/**
 * @brief Command data for the pause_task command.
 */
typedef struct
{
  char *task_id;   ///< ID of task to pause.
} pause_task_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
pause_task_data_reset (pause_task_data_t *data)
{
  free (data->task_id);

  memset (data, 0, sizeof (pause_task_data_t));
}

/**
 * @brief Command data for the resume_or_start_task command.
 */
typedef struct
{
  char *task_id;   ///< ID of task to resume or start.
} resume_or_start_task_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
resume_or_start_task_data_reset (resume_or_start_task_data_t *data)
{
  free (data->task_id);

  memset (data, 0, sizeof (resume_or_start_task_data_t));
}

/**
 * @brief Command data for the resume_paused_task command.
 */
typedef struct
{
  char *task_id;   ///< ID of paused task to resume.
} resume_paused_task_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
resume_paused_task_data_reset (resume_paused_task_data_t *data)
{
  free (data->task_id);

  memset (data, 0, sizeof (resume_paused_task_data_t));
}

/**
 * @brief Command data for the resume_stopped_task command.
 */
typedef struct
{
  char *task_id;   ///< ID of stopped task to resume.
} resume_stopped_task_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
resume_stopped_task_data_reset (resume_stopped_task_data_t *data)
{
  free (data->task_id);

  memset (data, 0, sizeof (resume_stopped_task_data_t));
}

/**
 * @brief Command data for the start_task command.
 */
typedef struct
{
  char *task_id;   ///< ID of task to start.
} start_task_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
start_task_data_reset (start_task_data_t *data)
{
  free (data->task_id);

  memset (data, 0, sizeof (start_task_data_t));
}

/**
 * @brief Command data for the stop_task command.
 */
typedef struct
{
  char *task_id;   ///< ID of task to stop.
} stop_task_data_t;

/**
 * @brief Free members of a stop_task_data_t and set them to NULL.
 */
/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
stop_task_data_reset (stop_task_data_t *data)
{
  free (data->task_id);

  memset (data, 0, sizeof (stop_task_data_t));
}

/**
 * @brief Command data for the test_escalator command.
 */
typedef struct
{
  char *escalator_id;   ///< ID of escalator to test.
} test_escalator_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
test_escalator_data_reset (test_escalator_data_t *data)
{
  free (data->escalator_id);

  memset (data, 0, sizeof (test_escalator_data_t));
}

/**
 * @brief Command data for the verify_agent command.
 */
typedef struct
{
  char *agent_id;   ///< ID of agent to verify.
} verify_agent_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
verify_agent_data_reset (verify_agent_data_t *data)
{
  free (data->agent_id);

  memset (data, 0, sizeof (verify_agent_data_t));
}

/**
 * @brief Command data for the verify_report_format command.
 */
typedef struct
{
  char *report_format_id;   ///< ID of report format to verify.
} verify_report_format_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
verify_report_format_data_reset (verify_report_format_data_t *data)
{
  free (data->report_format_id);

  memset (data, 0, sizeof (verify_report_format_data_t));
}

/**
 * @brief Command data, as passed between OMP parser callbacks.
 */
typedef union
{
  create_agent_data_t create_agent;                   ///< create_agent
  create_config_data_t create_config;                 ///< create_config
  create_escalator_data_t create_escalator;           ///< create_escalator
  create_lsc_credential_data_t create_lsc_credential; ///< create_lsc_credential
  create_note_data_t create_note;                     ///< create_note
  create_override_data_t create_override;             ///< create_override
  create_report_format_data_t create_report_format;   ///< create_report_format
  create_schedule_data_t create_schedule;             ///< create_schedule
  create_slave_data_t create_slave;                   ///< create_slave
  create_target_data_t create_target;                 ///< create_target
  create_task_data_t create_task;                     ///< create_task
  delete_agent_data_t delete_agent;                   ///< delete_agent
  delete_config_data_t delete_config;                 ///< delete_config
  delete_escalator_data_t delete_escalator;           ///< delete_escalator
  delete_lsc_credential_data_t delete_lsc_credential; ///< delete_lsc_credential
  delete_note_data_t delete_note;                     ///< delete_note
  delete_override_data_t delete_override;             ///< delete_override
  delete_report_data_t delete_report;                 ///< delete_report
  delete_report_format_data_t delete_report_format;   ///< delete_report_format
  delete_schedule_data_t delete_schedule;             ///< delete_schedule
  delete_slave_data_t delete_slave;                   ///< delete_slave
  delete_target_data_t delete_target;                 ///< delete_target
  delete_task_data_t delete_task;                     ///< delete_task
  get_agents_data_t get_agents;                       ///< get_agents
  get_configs_data_t get_configs;                     ///< get_configs
  get_dependencies_data_t get_dependencies;           ///< get_dependencies
  get_escalators_data_t get_escalators;               ///< get_escalators
  get_lsc_credentials_data_t get_lsc_credentials;     ///< get_lsc_credentials
  get_notes_data_t get_notes;                         ///< get_notes
  get_nvts_data_t get_nvts;                           ///< get_nvts
  get_nvt_families_data_t get_nvt_families;           ///< get_nvt_families
  get_nvt_feed_checksum_data_t get_nvt_feed_checksum; ///< get_nvt_feed_checksum
  get_overrides_data_t get_overrides;                 ///< get_overrides
  get_preferences_data_t get_preferences;             ///< get_preferences
  get_reports_data_t get_reports;                     ///< get_reports
  get_report_formats_data_t get_report_formats;       ///< get_report_formats
  get_results_data_t get_results;                     ///< get_results
  get_schedules_data_t get_schedules;                 ///< get_schedules
  get_slaves_data_t get_slaves;                       ///< get_slaves
  get_system_reports_data_t get_system_reports;       ///< get_system_reports
  get_targets_data_t get_targets;                     ///< get_targets
  get_tasks_data_t get_tasks;                         ///< get_tasks
  modify_config_data_t modify_config;                 ///< modify_config
  modify_report_data_t modify_report;                 ///< modify_report
  modify_report_format_data_t modify_report_format;   ///< modify_report_format
  modify_task_data_t modify_task;                     ///< modify_task
  pause_task_data_t pause_task;                       ///< pause_task
  resume_or_start_task_data_t resume_or_start_task;   ///< resume_or_start_task
  resume_paused_task_data_t resume_paused_task;       ///< resume_paused_task
  resume_stopped_task_data_t resume_stopped_task;     ///< resume_stopped_task
  start_task_data_t start_task;                       ///< start_task
  stop_task_data_t stop_task;                         ///< stop_task
  test_escalator_data_t test_escalator;               ///< test_escalator
  verify_agent_data_t verify_agent;                   ///< verify_agent
  verify_report_format_data_t verify_report_format;   ///< verify_report_format
} command_data_t;

/**
 * @brief Initialise command data.
 */
static void
command_data_init (command_data_t *data)
{
  memset (data, 0, sizeof (command_data_t));
}


/* Global variables. */

/**
 * @brief Parser callback data.
 */
command_data_t command_data;

/**
 * @brief Parser callback data for CREATE_AGENT.
 */
create_agent_data_t *create_agent_data
 = (create_agent_data_t*) &(command_data.create_agent);

/**
 * @brief Parser callback data for CREATE_CONFIG.
 */
create_config_data_t *create_config_data
 = (create_config_data_t*) &(command_data.create_config);

/**
 * @brief Parser callback data for CREATE_ESCALATOR.
 */
create_escalator_data_t *create_escalator_data
 = (create_escalator_data_t*) &(command_data.create_escalator);

/**
 * @brief Parser callback data for CREATE_LSC_CREDENTIAL.
 */
create_lsc_credential_data_t *create_lsc_credential_data
 = (create_lsc_credential_data_t*) &(command_data.create_lsc_credential);

/**
 * @brief Parser callback data for CREATE_NOTE.
 */
create_note_data_t *create_note_data
 = (create_note_data_t*) &(command_data.create_note);

/**
 * @brief Parser callback data for CREATE_OVERRIDE.
 */
create_override_data_t *create_override_data
 = (create_override_data_t*) &(command_data.create_override);

/**
 * @brief Parser callback data for CREATE_REPORT_FORMAT.
 */
create_report_format_data_t *create_report_format_data
 = (create_report_format_data_t*) &(command_data.create_report_format);

/**
 * @brief Parser callback data for CREATE_SCHEDULE.
 */
create_schedule_data_t *create_schedule_data
 = (create_schedule_data_t*) &(command_data.create_schedule);

/**
 * @brief Parser callback data for CREATE_SLAVE.
 */
create_slave_data_t *create_slave_data
 = (create_slave_data_t*) &(command_data.create_slave);

/**
 * @brief Parser callback data for CREATE_TARGET.
 */
create_target_data_t *create_target_data
 = (create_target_data_t*) &(command_data.create_target);

/**
 * @brief Parser callback data for CREATE_TASK.
 */
create_task_data_t *create_task_data
 = (create_task_data_t*) &(command_data.create_task);

/**
 * @brief Parser callback data for DELETE_AGENT.
 */
delete_agent_data_t *delete_agent_data
 = (delete_agent_data_t*) &(command_data.delete_agent);

/**
 * @brief Parser callback data for DELETE_CONFIG.
 */
delete_config_data_t *delete_config_data
 = (delete_config_data_t*) &(command_data.delete_config);

/**
 * @brief Parser callback data for DELETE_ESCALATOR.
 */
delete_escalator_data_t *delete_escalator_data
 = (delete_escalator_data_t*) &(command_data.delete_escalator);

/**
 * @brief Parser callback data for DELETE_LSC_CREDENTIAL.
 */
delete_lsc_credential_data_t *delete_lsc_credential_data
 = (delete_lsc_credential_data_t*) &(command_data.delete_lsc_credential);

/**
 * @brief Parser callback data for DELETE_NOTE.
 */
delete_note_data_t *delete_note_data
 = (delete_note_data_t*) &(command_data.delete_note);

/**
 * @brief Parser callback data for DELETE_OVERRIDE.
 */
delete_override_data_t *delete_override_data
 = (delete_override_data_t*) &(command_data.delete_override);

/**
 * @brief Parser callback data for DELETE_REPORT.
 */
delete_report_data_t *delete_report_data
 = (delete_report_data_t*) &(command_data.delete_report);

/**
 * @brief Parser callback data for DELETE_REPORT_FORMAT.
 */
delete_report_format_data_t *delete_report_format_data
 = (delete_report_format_data_t*) &(command_data.delete_report_format);

/**
 * @brief Parser callback data for DELETE_SCHEDULE.
 */
delete_schedule_data_t *delete_schedule_data
 = (delete_schedule_data_t*) &(command_data.delete_schedule);

/**
 * @brief Parser callback data for DELETE_SLAVE.
 */
delete_slave_data_t *delete_slave_data
 = (delete_slave_data_t*) &(command_data.delete_slave);

/**
 * @brief Parser callback data for DELETE_TARGET.
 */
delete_target_data_t *delete_target_data
 = (delete_target_data_t*) &(command_data.delete_target);

/**
 * @brief Parser callback data for DELETE_TASK.
 */
delete_task_data_t *delete_task_data
 = (delete_task_data_t*) &(command_data.delete_task);

/**
 * @brief Parser callback data for GET_AGENTS.
 */
get_agents_data_t *get_agents_data
 = &(command_data.get_agents);

/**
 * @brief Parser callback data for GET_CONFIGS.
 */
get_configs_data_t *get_configs_data
 = &(command_data.get_configs);

/**
 * @brief Parser callback data for GET_DEPENDENCIES.
 */
get_dependencies_data_t *get_dependencies_data
 = &(command_data.get_dependencies);

/**
 * @brief Parser callback data for GET_ESCALATORS.
 */
get_escalators_data_t *get_escalators_data
 = &(command_data.get_escalators);

/**
 * @brief Parser callback data for GET_LSC_CREDENTIALS.
 */
get_lsc_credentials_data_t *get_lsc_credentials_data
 = &(command_data.get_lsc_credentials);

/**
 * @brief Parser callback data for GET_NOTES.
 */
get_notes_data_t *get_notes_data
 = &(command_data.get_notes);

/**
 * @brief Parser callback data for GET_NVTS.
 */
get_nvts_data_t *get_nvts_data
 = &(command_data.get_nvts);

/**
 * @brief Parser callback data for GET_NVT_FAMILIES.
 */
get_nvt_families_data_t *get_nvt_families_data
 = &(command_data.get_nvt_families);

/**
 * @brief Parser callback data for GET_NVT_FEED_CHECKSUM.
 */
get_nvt_feed_checksum_data_t *get_nvt_feed_checksum_data
 = &(command_data.get_nvt_feed_checksum);

/**
 * @brief Parser callback data for GET_OVERRIDES.
 */
get_overrides_data_t *get_overrides_data
 = &(command_data.get_overrides);

/**
 * @brief Parser callback data for GET_PREFERENCES.
 */
get_preferences_data_t *get_preferences_data
 = &(command_data.get_preferences);

/**
 * @brief Parser callback data for GET_REPORTS.
 */
get_reports_data_t *get_reports_data
 = &(command_data.get_reports);

/**
 * @brief Parser callback data for GET_REPORT_FORMATS.
 */
get_report_formats_data_t *get_report_formats_data
 = &(command_data.get_report_formats);

/**
 * @brief Parser callback data for GET_RESULTS.
 */
get_results_data_t *get_results_data
 = &(command_data.get_results);

/**
 * @brief Parser callback data for GET_SCHEDULES.
 */
get_schedules_data_t *get_schedules_data
 = &(command_data.get_schedules);

/**
 * @brief Parser callback data for GET_SLAVES.
 */
get_slaves_data_t *get_slaves_data
 = &(command_data.get_slaves);

/**
 * @brief Parser callback data for GET_SYSTEM_REPORTS.
 */
get_system_reports_data_t *get_system_reports_data
 = &(command_data.get_system_reports);

/**
 * @brief Parser callback data for GET_TARGETS.
 */
get_targets_data_t *get_targets_data
 = &(command_data.get_targets);

/**
 * @brief Parser callback data for GET_TASKS.
 */
get_tasks_data_t *get_tasks_data
 = &(command_data.get_tasks);

/**
 * @brief Parser callback data for CREATE_CONFIG (import).
 */
import_config_data_t *import_config_data
 = (import_config_data_t*) &(command_data.create_config.import);

/**
 * @brief Parser callback data for MODIFY_CONFIG.
 */
modify_config_data_t *modify_config_data
 = &(command_data.modify_config);

/**
 * @brief Parser callback data for MODIFY_NOTE.
 */
modify_note_data_t *modify_note_data
 = (modify_note_data_t*) &(command_data.create_note);

/**
 * @brief Parser callback data for MODIFY_OVERRIDE.
 */
modify_override_data_t *modify_override_data
 = (modify_override_data_t*) &(command_data.create_override);

/**
 * @brief Parser callback data for MODIFY_REPORT.
 */
modify_report_data_t *modify_report_data
 = &(command_data.modify_report);

/**
 * @brief Parser callback data for MODIFY_REPORT_FORMAT.
 */
modify_report_format_data_t *modify_report_format_data
 = &(command_data.modify_report_format);

/**
 * @brief Parser callback data for MODIFY_TASK.
 */
modify_task_data_t *modify_task_data
 = &(command_data.modify_task);

/**
 * @brief Parser callback data for PAUSE_TASK.
 */
pause_task_data_t *pause_task_data
 = (pause_task_data_t*) &(command_data.pause_task);

/**
 * @brief Parser callback data for RESUME_OR_START_TASK.
 */
resume_or_start_task_data_t *resume_or_start_task_data
 = (resume_or_start_task_data_t*) &(command_data.resume_or_start_task);

/**
 * @brief Parser callback data for RESUME_PAUSED_TASK.
 */
resume_paused_task_data_t *resume_paused_task_data
 = (resume_paused_task_data_t*) &(command_data.resume_paused_task);

/**
 * @brief Parser callback data for RESUME_STOPPED_TASK.
 */
resume_stopped_task_data_t *resume_stopped_task_data
 = (resume_stopped_task_data_t*) &(command_data.resume_stopped_task);

/**
 * @brief Parser callback data for START_TASK.
 */
start_task_data_t *start_task_data
 = (start_task_data_t*) &(command_data.start_task);

/**
 * @brief Parser callback data for STOP_TASK.
 */
stop_task_data_t *stop_task_data
 = (stop_task_data_t*) &(command_data.stop_task);

/**
 * @brief Parser callback data for TEST_ESCALATOR.
 */
test_escalator_data_t *test_escalator_data
 = (test_escalator_data_t*) &(command_data.test_escalator);

/**
 * @brief Parser callback data for VERIFY_AGENT.
 */
verify_agent_data_t *verify_agent_data
 = (verify_agent_data_t*) &(command_data.verify_agent);

/**
 * @brief Parser callback data for VERIFY_REPORT_FORMAT.
 */
verify_report_format_data_t *verify_report_format_data
 = (verify_report_format_data_t*) &(command_data.verify_report_format);

/**
 * @brief Hack for returning forked process status from the callbacks.
 */
int current_error;

/**
 * @brief Hack for returning fork status to caller.
 */
int forked;

/**
 * @brief Buffer of output to the client.
 */
char to_client[TO_CLIENT_BUFFER_SIZE];

/**
 * @brief The start of the data in the \ref to_client buffer.
 */
buffer_size_t to_client_start = 0;
/**
 * @brief The end of the data in the \ref to_client buffer.
 */
buffer_size_t to_client_end = 0;

/**
 * @brief Client input parsing context.
 */
static /*@null@*/ /*@only@*/ GMarkupParseContext*
xml_context = NULL;

/**
 * @brief Client input parser.
 */
static GMarkupParser xml_parser;


/* Client state. */

/**
 * @brief Possible states of the client.
 */
typedef enum
{
  CLIENT_TOP,
  CLIENT_AUTHENTIC,

  CLIENT_AUTHENTICATE,
  CLIENT_AUTHENTICATE_CREDENTIALS,
  CLIENT_AUTHENTICATE_CREDENTIALS_PASSWORD,
  CLIENT_AUTHENTICATE_CREDENTIALS_USERNAME,
  CLIENT_AUTHENTIC_COMMANDS,
  CLIENT_COMMANDS,
  CLIENT_CREATE_AGENT,
  CLIENT_CREATE_AGENT_NAME,
  CLIENT_CREATE_AGENT_COMMENT,
  CLIENT_CREATE_AGENT_INSTALLER,
  CLIENT_CREATE_AGENT_INSTALLER_FILENAME,
  CLIENT_CREATE_AGENT_INSTALLER_SIGNATURE,
  CLIENT_CREATE_AGENT_HOWTO_INSTALL,
  CLIENT_CREATE_AGENT_HOWTO_USE,
  CLIENT_CREATE_CONFIG,
  CLIENT_CREATE_CONFIG_COMMENT,
  CLIENT_CREATE_CONFIG_COPY,
  CLIENT_CREATE_CONFIG_NAME,
  CLIENT_CREATE_CONFIG_RCFILE,
  /* get_configs_response (GCR) is used for config export.  CLIENT_C_C is
   * for CLIENT_CREATE_CONFIG. */
  CLIENT_C_C_GCR,
  CLIENT_C_C_GCR_CONFIG,
  CLIENT_C_C_GCR_CONFIG_COMMENT,
  CLIENT_C_C_GCR_CONFIG_NAME,
  CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS,
  CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR,
  CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_NAME,
  CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_INCLUDE,
  CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_TYPE,
  CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_FAMILY_OR_NVT,
  CLIENT_C_C_GCR_CONFIG_PREFERENCES,
  CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE,
  CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_ALT,
  CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NAME,
  CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NVT,
  CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NVT_NAME,
  CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_TYPE,
  CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_VALUE,
  CLIENT_CREATE_ESCALATOR,
  CLIENT_CREATE_ESCALATOR_COMMENT,
  CLIENT_CREATE_ESCALATOR_CONDITION,
  CLIENT_CREATE_ESCALATOR_CONDITION_DATA,
  CLIENT_CREATE_ESCALATOR_CONDITION_DATA_NAME,
  CLIENT_CREATE_ESCALATOR_EVENT,
  CLIENT_CREATE_ESCALATOR_EVENT_DATA,
  CLIENT_CREATE_ESCALATOR_EVENT_DATA_NAME,
  CLIENT_CREATE_ESCALATOR_METHOD,
  CLIENT_CREATE_ESCALATOR_METHOD_DATA,
  CLIENT_CREATE_ESCALATOR_METHOD_DATA_NAME,
  CLIENT_CREATE_ESCALATOR_NAME,
  CLIENT_CREATE_LSC_CREDENTIAL,
  CLIENT_CREATE_LSC_CREDENTIAL_COMMENT,
  CLIENT_CREATE_LSC_CREDENTIAL_NAME,
  CLIENT_CREATE_LSC_CREDENTIAL_PASSWORD,
  CLIENT_CREATE_LSC_CREDENTIAL_LOGIN,
  CLIENT_CREATE_NOTE,
  CLIENT_CREATE_NOTE_HOSTS,
  CLIENT_CREATE_NOTE_NVT,
  CLIENT_CREATE_NOTE_PORT,
  CLIENT_CREATE_NOTE_RESULT,
  CLIENT_CREATE_NOTE_TASK,
  CLIENT_CREATE_NOTE_TEXT,
  CLIENT_CREATE_NOTE_THREAT,
  CLIENT_CREATE_OVERRIDE,
  CLIENT_CREATE_OVERRIDE_HOSTS,
  CLIENT_CREATE_OVERRIDE_NEW_THREAT,
  CLIENT_CREATE_OVERRIDE_NVT,
  CLIENT_CREATE_OVERRIDE_PORT,
  CLIENT_CREATE_OVERRIDE_RESULT,
  CLIENT_CREATE_OVERRIDE_TASK,
  CLIENT_CREATE_OVERRIDE_TEXT,
  CLIENT_CREATE_OVERRIDE_THREAT,
  CLIENT_CREATE_REPORT_FORMAT,
  /* get_report_formats (GRF) is used for report format export.  CLIENT_CRF is
   * for CLIENT_CREATE_REPORT_FORMAT. */
  CLIENT_CRF_GRFR,
  CLIENT_CRF_GRFR_REPORT_FORMAT,
  CLIENT_CRF_GRFR_REPORT_FORMAT_CONTENT_TYPE,
  CLIENT_CRF_GRFR_REPORT_FORMAT_DESCRIPTION,
  CLIENT_CRF_GRFR_REPORT_FORMAT_EXTENSION,
  CLIENT_CRF_GRFR_REPORT_FORMAT_FILE,
  CLIENT_CRF_GRFR_REPORT_FORMAT_GLOBAL,
  CLIENT_CRF_GRFR_REPORT_FORMAT_NAME,
  CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM,
  CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_NAME,
  CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_VALUE,
  CLIENT_CRF_GRFR_REPORT_FORMAT_SIGNATURE,
  CLIENT_CRF_GRFR_REPORT_FORMAT_SUMMARY,
  CLIENT_CRF_GRFR_REPORT_FORMAT_TRUST,
  CLIENT_CREATE_SCHEDULE,
  CLIENT_CREATE_SCHEDULE_NAME,
  CLIENT_CREATE_SCHEDULE_COMMENT,
  CLIENT_CREATE_SCHEDULE_FIRST_TIME,
  CLIENT_CREATE_SCHEDULE_FIRST_TIME_DAY_OF_MONTH,
  CLIENT_CREATE_SCHEDULE_FIRST_TIME_HOUR,
  CLIENT_CREATE_SCHEDULE_FIRST_TIME_MINUTE,
  CLIENT_CREATE_SCHEDULE_FIRST_TIME_MONTH,
  CLIENT_CREATE_SCHEDULE_FIRST_TIME_YEAR,
  CLIENT_CREATE_SCHEDULE_DURATION,
  CLIENT_CREATE_SCHEDULE_DURATION_UNIT,
  CLIENT_CREATE_SCHEDULE_PERIOD,
  CLIENT_CREATE_SCHEDULE_PERIOD_UNIT,
  CLIENT_CREATE_SLAVE,
  CLIENT_CREATE_SLAVE_COMMENT,
  CLIENT_CREATE_SLAVE_HOST,
  CLIENT_CREATE_SLAVE_LOGIN,
  CLIENT_CREATE_SLAVE_NAME,
  CLIENT_CREATE_SLAVE_PASSWORD,
  CLIENT_CREATE_SLAVE_PORT,
  CLIENT_CREATE_TARGET,
  CLIENT_CREATE_TARGET_COMMENT,
  CLIENT_CREATE_TARGET_HOSTS,
  CLIENT_CREATE_TARGET_LSC_CREDENTIAL,
  CLIENT_CREATE_TARGET_NAME,
  CLIENT_CREATE_TARGET_TARGET_LOCATOR,
  CLIENT_CREATE_TARGET_TARGET_LOCATOR_PASSWORD,
  CLIENT_CREATE_TARGET_TARGET_LOCATOR_USERNAME,
  CLIENT_CREATE_TASK,
  CLIENT_CREATE_TASK_COMMENT,
  CLIENT_CREATE_TASK_CONFIG,
  CLIENT_CREATE_TASK_ESCALATOR,
  CLIENT_CREATE_TASK_NAME,
  CLIENT_CREATE_TASK_RCFILE,
  CLIENT_CREATE_TASK_SCHEDULE,
  CLIENT_CREATE_TASK_SLAVE,
  CLIENT_CREATE_TASK_TARGET,
  CLIENT_DELETE_AGENT,
  CLIENT_DELETE_CONFIG,
  CLIENT_DELETE_ESCALATOR,
  CLIENT_DELETE_LSC_CREDENTIAL,
  CLIENT_DELETE_NOTE,
  CLIENT_DELETE_OVERRIDE,
  CLIENT_DELETE_REPORT,
  CLIENT_DELETE_REPORT_FORMAT,
  CLIENT_DELETE_SCHEDULE,
  CLIENT_DELETE_SLAVE,
  CLIENT_DELETE_TASK,
  CLIENT_DELETE_TARGET,
  CLIENT_GET_AGENTS,
#if 0
  CLIENT_GET_CERTIFICATES,
#endif
  CLIENT_GET_CONFIGS,
  CLIENT_GET_DEPENDENCIES,
  CLIENT_GET_ESCALATORS,
  CLIENT_GET_LSC_CREDENTIALS,
  CLIENT_GET_NOTES,
  CLIENT_GET_NVTS,
  CLIENT_GET_NVT_FAMILIES,
  CLIENT_GET_NVT_FEED_CHECKSUM,
  CLIENT_GET_OVERRIDES,
  CLIENT_GET_PREFERENCES,
  CLIENT_GET_REPORTS,
  CLIENT_GET_REPORT_FORMATS,
  CLIENT_GET_RESULTS,
  CLIENT_GET_SCHEDULES,
  CLIENT_GET_SLAVES,
  CLIENT_GET_SYSTEM_REPORTS,
  CLIENT_GET_TARGET_LOCATORS,
  CLIENT_GET_TARGETS,
  CLIENT_GET_TASKS,
  CLIENT_GET_VERSION,
  CLIENT_GET_VERSION_AUTHENTIC,
  CLIENT_HELP,
  CLIENT_MODIFY_REPORT,
  CLIENT_MODIFY_REPORT_COMMENT,
  CLIENT_MODIFY_REPORT_FORMAT,
  CLIENT_MODIFY_REPORT_FORMAT_NAME,
  CLIENT_MODIFY_REPORT_FORMAT_SUMMARY,
  CLIENT_MODIFY_CONFIG,
  CLIENT_MODIFY_CONFIG_PREFERENCE,
  CLIENT_MODIFY_CONFIG_PREFERENCE_NAME,
  CLIENT_MODIFY_CONFIG_PREFERENCE_NVT,
  CLIENT_MODIFY_CONFIG_PREFERENCE_VALUE,
  CLIENT_MODIFY_CONFIG_FAMILY_SELECTION,
  CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY,
  CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_ALL,
  CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_GROWING,
  CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_NAME,
  CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_GROWING,
  CLIENT_MODIFY_CONFIG_NVT_SELECTION,
  CLIENT_MODIFY_CONFIG_NVT_SELECTION_FAMILY,
  CLIENT_MODIFY_CONFIG_NVT_SELECTION_NVT,
  CLIENT_MODIFY_NOTE,
  CLIENT_MODIFY_NOTE_HOSTS,
  CLIENT_MODIFY_NOTE_PORT,
  CLIENT_MODIFY_NOTE_RESULT,
  CLIENT_MODIFY_NOTE_TASK,
  CLIENT_MODIFY_NOTE_TEXT,
  CLIENT_MODIFY_NOTE_THREAT,
  CLIENT_MODIFY_OVERRIDE,
  CLIENT_MODIFY_OVERRIDE_HOSTS,
  CLIENT_MODIFY_OVERRIDE_NEW_THREAT,
  CLIENT_MODIFY_OVERRIDE_PORT,
  CLIENT_MODIFY_OVERRIDE_RESULT,
  CLIENT_MODIFY_OVERRIDE_TASK,
  CLIENT_MODIFY_OVERRIDE_TEXT,
  CLIENT_MODIFY_OVERRIDE_THREAT,
  CLIENT_MODIFY_TASK,
  CLIENT_MODIFY_TASK_COMMENT,
  CLIENT_MODIFY_TASK_ESCALATOR,
  CLIENT_MODIFY_TASK_FILE,
  CLIENT_MODIFY_TASK_NAME,
  CLIENT_MODIFY_TASK_RCFILE,
  CLIENT_MODIFY_TASK_SCHEDULE,
  CLIENT_PAUSE_TASK,
  CLIENT_RESUME_OR_START_TASK,
  CLIENT_RESUME_PAUSED_TASK,
  CLIENT_RESUME_STOPPED_TASK,
  CLIENT_START_TASK,
  CLIENT_STOP_TASK,
  CLIENT_TEST_ESCALATOR,
  CLIENT_VERIFY_AGENT,
  CLIENT_VERIFY_REPORT_FORMAT
} client_state_t;

/**
 * @brief The state of the client.
 */
static client_state_t client_state = CLIENT_TOP;

/**
 * @brief Set the client state.
 */
static void
set_client_state (client_state_t state)
{
  client_state = state;
  tracef ("   client state set: %i\n", client_state);
}


/* Communication. */

/**
 * @brief Send a response message to the client.
 *
 * Queue a message in \ref to_client.
 *
 * @param[in]  msg              The message, a string.
 * @param[in]  write_to_client       Function to write to client.
 * @param[in]  write_to_client_data  Argument to \p write_to_client.
 *
 * @return TRUE if write to client failed, else FALSE.
 */
static gboolean
send_to_client (const char* msg, int (*write_to_client) (void*),
                void* write_to_client_data)
{
  assert (to_client_end <= TO_CLIENT_BUFFER_SIZE);
  assert (msg);
  assert (write_to_client);

  while (((buffer_size_t) TO_CLIENT_BUFFER_SIZE) - to_client_end
         < strlen (msg))
    {
      buffer_size_t length;

      /* Too little space in to_client buffer for message. */

      switch (write_to_client (write_to_client_data))
        {
          case  0:      /* Wrote everything in to_client. */
            break;
          case -1:      /* Error. */
            tracef ("   send_to_client full (%i < %zu); client write failed\n",
                    ((buffer_size_t) TO_CLIENT_BUFFER_SIZE) - to_client_end,
                    strlen (msg));
            return TRUE;
          case -2:      /* Wrote as much as client was willing to accept. */
            break;
          default:      /* Programming error. */
            assert (0);
        }

      length = ((buffer_size_t) TO_CLIENT_BUFFER_SIZE) - to_client_end;

      if (length > strlen (msg))
        break;

      memmove (to_client + to_client_end, msg, length);
      tracef ("-> client: %.*s\n", (int) length, msg);
      to_client_end += length;
      msg += length;
    }

  if (strlen (msg))
    {
      assert (strlen (msg)
              <= (((buffer_size_t) TO_CLIENT_BUFFER_SIZE) - to_client_end));
      memmove (to_client + to_client_end, msg, strlen (msg));
      tracef ("-> client: %s\n", msg);
      to_client_end += strlen (msg);
    }

  return FALSE;
}

/**
 * @brief Send an XML element error response message to the client.
 *
 * @param[in]  command  Command name.
 * @param[in]  element  Element name.
 * @param[in]  write_to_client       Function to write to client.
 * @param[in]  write_to_client_data  Argument to \p write_to_client.
 *
 * @return TRUE if out of space in to_client, else FALSE.
 */
static gboolean
send_element_error_to_client (const char* command, const char* element,
                              int (*write_to_client) (void*),
                              void* write_to_client_data)
{
  gchar *msg;
  gboolean ret;

  /** @todo Set gerror so parsing terminates. */
  msg = g_strdup_printf ("<%s_response status=\""
                         STATUS_ERROR_SYNTAX
                         "\" status_text=\"Bogus element: %s\"/>",
                         command,
                         element);
  ret = send_to_client (msg, write_to_client, write_to_client_data);
  g_free (msg);
  return ret;
}

/**
 * @brief Send an XML find error response message to the client.
 *
 * @param[in]  command  Command name.
 * @param[in]  type     Resource type.
 * @param[in]  id       Resource ID.
 * @param[in]  write_to_client       Function to write to client.
 * @param[in]  write_to_client_data  Argument to \p write_to_client.
 *
 * @return TRUE if out of space in to_client, else FALSE.
 */
static gboolean
send_find_error_to_client (const char* command, const char* type,
                           const char* id, int (*write_to_client) (void*),
                           void* write_to_client_data)
{
  gchar *msg;
  gboolean ret;

  msg = g_strdup_printf ("<%s_response status=\""
                         STATUS_ERROR_MISSING
                         "\" status_text=\"Failed to find %s '%s'\"/>",
                         command, type, id);
  ret = send_to_client (msg, write_to_client, write_to_client_data);
  g_free (msg);
  return ret;
}

/**
 * @brief Set an out of space parse error on a GError.
 *
 * @param [out]  error  The error.
 */
static void
error_send_to_client (GError** error)
{
  tracef ("   send_to_client out of space in to_client\n");
  g_set_error (error, G_MARKUP_ERROR, G_MARKUP_ERROR_PARSE,
               "Manager out of space for reply to client.");
}

/**
 * @brief Set an internal error on a GError.
 *
 * @param [out]  error  The error.
 */
static void
internal_error_send_to_client (GError** error)
{
  g_set_error (error, G_MARKUP_ERROR, G_MARKUP_ERROR_PARSE,
               "Internal Error.");
}


/* XML parser handlers. */

/**
 * @brief Expand to XML for a STATUS_ERROR_SYNTAX response.
 *
 * @param  tag   Name of the command generating the response.
 * @param  text  Text for the status_text attribute of the response.
 */
#define XML_ERROR_SYNTAX(tag, text)                      \
 "<" tag "_response"                                     \
 " status=\"" STATUS_ERROR_SYNTAX "\""                   \
 " status_text=\"" text "\"/>"

/**
 * @brief Expand to XML for a STATUS_ERROR_ACCESS response.
 *
 * @param  tag  Name of the command generating the response.
 */
#define XML_ERROR_ACCESS(tag)                            \
 "<" tag "_response"                                     \
 " status=\"" STATUS_ERROR_ACCESS "\""                   \
 " status_text=\"" STATUS_ERROR_ACCESS_TEXT "\"/>"

/**
 * @brief Expand to XML for a STATUS_ERROR_MISSING response.
 *
 * @param  tag  Name of the command generating the response.
 */
#define XML_ERROR_MISSING(tag)                           \
 "<" tag "_response"                                     \
 " status=\"" STATUS_ERROR_MISSING "\""                  \
 " status_text=\"" STATUS_ERROR_MISSING_TEXT "\"/>"

/**
 * @brief Expand to XML for a STATUS_ERROR_AUTH_FAILED response.
 *
 * @param  tag  Name of the command generating the response.
 */
#define XML_ERROR_AUTH_FAILED(tag)                       \
 "<" tag "_response"                                     \
 " status=\"" STATUS_ERROR_AUTH_FAILED "\""              \
 " status_text=\"" STATUS_ERROR_AUTH_FAILED_TEXT "\"/>"

/**
 * @brief Expand to XML for a STATUS_OK response.
 *
 * @param  tag  Name of the command generating the response.
 */
#define XML_OK(tag)                                      \
 "<" tag "_response"                                     \
 " status=\"" STATUS_OK "\""                             \
 " status_text=\"" STATUS_OK_TEXT "\"/>"

/**
 * @brief Expand to XML for a STATUS_OK_CREATED response.
 *
 * @param  tag  Name of the command generating the response.
 */
#define XML_OK_CREATED(tag)                              \
 "<" tag "_response"                                     \
 " status=\"" STATUS_OK_CREATED "\""                     \
 " status_text=\"" STATUS_OK_CREATED_TEXT "\"/>"

/**
 * @brief Expand to XML for a STATUS_OK_CREATED response with %s for ID.
 *
 * @param  tag  Name of the command generating the response.
 */
#define XML_OK_CREATED_ID(tag)                           \
 "<" tag "_response"                                     \
 " status=\"" STATUS_OK_CREATED "\""                     \
 " status_text=\"" STATUS_OK_CREATED_TEXT "\""           \
 " id=\"%s\"/>"

/**
 * @brief Expand to XML for a STATUS_OK_REQUESTED response.
 *
 * @param  tag  Name of the command generating the response.
 */
#define XML_OK_REQUESTED(tag)                            \
 "<" tag "_response"                                     \
 " status=\"" STATUS_OK_REQUESTED "\""                   \
 " status_text=\"" STATUS_OK_REQUESTED_TEXT "\"/>"

/**
 * @brief Expand to XML for a STATUS_INTERNAL_ERROR response.
 *
 * @param  tag  Name of the command generating the response.
 */
#define XML_INTERNAL_ERROR(tag)                          \
 "<" tag "_response"                                     \
 " status=\"" STATUS_INTERNAL_ERROR "\""                 \
 " status_text=\"" STATUS_INTERNAL_ERROR_TEXT "\"/>"

/**
 * @brief Expand to XML for a STATUS_SERVICE_DOWN response.
 *
 * @param  tag  Name of the command generating the response.
 */
#define XML_SERVICE_DOWN(tag)                            \
 "<" tag "_response"                                     \
 " status=\"" STATUS_SERVICE_DOWN "\""                   \
 " status_text=\"" STATUS_SERVICE_DOWN_TEXT "\"/>"

/**
 * @brief Return number of hosts described by a hosts string.
 *
 * @param[in]  hosts  String describing hosts.
 *
 * @return Number of hosts, or -1 on error.
 */
int
max_hosts (const char *hosts)
{
  long count = 0;
  gchar** split = g_strsplit (hosts, ",", 0);
  gchar** point = split;

  /** @todo Check for errors in "hosts". */

  while (*point)
    {
      gchar* slash = strchr (*point, '/');
      if (slash)
        {
          slash++;
          if (*slash)
            {
              long int mask;
              struct in_addr addr;

              /* Convert text after slash to a bit netmask. */

              if (atoi (slash) > 32 && inet_aton (slash, &addr))
                {
                  in_addr_t haddr;

                  /* 192.168.200.0/255.255.255.252 */

                  haddr = ntohl (addr.s_addr);
                  mask = 32;
                  while ((haddr & 1) == 0)
                    {
                      mask--;
                      haddr = haddr >> 1;
                    }
                  if (mask < 8 || mask > 32) return -1;
                }
              else
                {
                  /* 192.168.200.0/30 */

                  errno = 0;
                  mask = strtol (slash, NULL, 10);
                  if (errno == ERANGE || mask < 8 || mask > 32) return -1;
                }

              /* Calculate number of hosts. */

              count += 1L << (32 - mask);
              /* Leave out the network and broadcast addresses. */
              if (mask < 31) count--;
            }
          else
            /* Just a trailing /. */
            count++;
        }
      else
        count++;
      point += 1;
    }
  return count;
}

/**
 * @brief Find an attribute in a parser callback list of attributes.
 *
 * @param[in]   attribute_names   List of names.
 * @param[in]   attribute_values  List of values.
 * @param[in]   attribute_name    Name of sought attribute.
 * @param[out]  attribute_value   Attribute value return.
 *
 * @return 1 if found, else 0.
 */
int
find_attribute (const gchar **attribute_names,
                const gchar **attribute_values,
                const char *attribute_name,
                const gchar **attribute_value)
{
  while (*attribute_names && *attribute_values)
    if (strcmp (*attribute_names, attribute_name))
      attribute_names++, attribute_values++;
    else
      {
        *attribute_value = *attribute_values;
        return 1;
      }
  return 0;
}

/**
 * @brief Find an attribute in a parser callback list of attributes and append
 * @brief it to a string using openvas_append_string.
 *
 * @param[in]   attribute_names   List of names.
 * @param[in]   attribute_values  List of values.
 * @param[in]   attribute_name    Name of sought attribute.
 * @param[out]  string            String to append attribute value to, if
 *                                found.
 *
 * @return 1 if found and appended, else 0.
 */
int
append_attribute (const gchar **attribute_names,
                  const gchar **attribute_values,
                  const char *attribute_name,
                  gchar **string)
{
  const gchar* attribute;
  if (find_attribute (attribute_names, attribute_values, attribute_name,
                      &attribute))
    {
      openvas_append_string (string, attribute);
      return 1;
    }
  return 0;
}

/** @cond STATIC */

/**
 * @brief Send response message to client, returning on fail.
 *
 * Queue a message in \ref to_client with \ref send_to_client.  On failure
 * call \ref error_send_to_client on a GError* called "error" and do a return.
 *
 * @param[in]   msg    The message, a string.
 */
#define SEND_TO_CLIENT_OR_FAIL(msg)                                          \
  do                                                                         \
    {                                                                        \
      if (send_to_client (msg, write_to_client, write_to_client_data))       \
        {                                                                    \
          error_send_to_client (error);                                      \
          return;                                                            \
        }                                                                    \
    }                                                                        \
  while (0)

/**
 * @brief Send response message to client, returning on fail.
 *
 * Queue a message in \ref to_client with \ref send_to_client.  On failure
 * call \ref error_send_to_client on a GError* called "error" and do a return.
 *
 * @param[in]   format    Format string for message.
 * @param[in]   args      Arguments for format string.
 */
#define SENDF_TO_CLIENT_OR_FAIL(format, args...)                             \
  do                                                                         \
    {                                                                        \
      gchar* msg = g_markup_printf_escaped (format , ## args);               \
      if (send_to_client (msg, write_to_client, write_to_client_data))       \
        {                                                                    \
          g_free (msg);                                                      \
          error_send_to_client (error);                                      \
          return;                                                            \
        }                                                                    \
      g_free (msg);                                                          \
    }                                                                        \
  while (0)

/** @endcond */

/** @todo Free globals when tags open, in case of duplicate tags. */
/**
 * @brief Handle the start of an OMP XML element.
 *
 * React to the start of an XML element according to the current value
 * of \ref client_state, usually adjusting \ref client_state to indicate
 * the change (with \ref set_client_state).  Call \ref send_to_client to
 * queue any responses for the client.
 *
 * Set error parameter on encountering an error.
 *
 * @param[in]  context           Parser context.
 * @param[in]  element_name      XML element name.
 * @param[in]  attribute_names   XML attribute name.
 * @param[in]  attribute_values  XML attribute values.
 * @param[in]  user_data         OMP parser.
 * @param[in]  error             Error parameter.
 */
static void
omp_xml_handle_start_element (/*@unused@*/ GMarkupParseContext* context,
                              const gchar *element_name,
                              const gchar **attribute_names,
                              const gchar **attribute_values,
                              gpointer user_data,
                              GError **error)
{
  omp_parser_t *omp_parser = (omp_parser_t*) user_data;
  int (*write_to_client) (void*) = (int (*) (void*)) omp_parser->client_writer;
  void* write_to_client_data = (void*) omp_parser->client_writer_data;

  tracef ("   XML  start: %s (%i)\n", element_name, client_state);

  switch (client_state)
    {
      case CLIENT_TOP:
        if (strcasecmp ("GET_VERSION", element_name) == 0)
          {
            set_client_state (CLIENT_GET_VERSION);
            break;
          }
        /*@fallthrough@*/
      case CLIENT_COMMANDS:
        if (strcasecmp ("AUTHENTICATE", element_name) == 0)
          {
            set_client_state (CLIENT_AUTHENTICATE);
          }
        else if (strcasecmp ("COMMANDS", element_name) == 0)
          {
            SENDF_TO_CLIENT_OR_FAIL
             ("<commands_response"
              " status=\"" STATUS_OK "\" status_text=\"" STATUS_OK_TEXT "\">");
            set_client_state (CLIENT_COMMANDS);
          }
        else
          {
            /** @todo If a real OMP command, return STATUS_ERROR_MUST_AUTH. */
            if (send_to_client
                 (XML_ERROR_SYNTAX ("omp",
                                    "First command must be AUTHENTICATE,"
                                    " COMMANDS or GET_VERSION"),
                  write_to_client,
                  write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            if (client_state == CLIENT_COMMANDS)
              send_to_client ("</commands_response>",
                              write_to_client,
                              write_to_client_data);
            g_set_error (error, G_MARKUP_ERROR, G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Must authenticate first.");
          }
        break;

      case CLIENT_AUTHENTIC:
      case CLIENT_AUTHENTIC_COMMANDS:
        if (strcasecmp ("AUTHENTICATE", element_name) == 0)
          {
            if (save_tasks ()) abort ();
            free_tasks ();
            free_credentials (&current_credentials);
            set_client_state (CLIENT_AUTHENTICATE);
          }
        else if (strcasecmp ("COMMANDS", element_name) == 0)
          {
            SEND_TO_CLIENT_OR_FAIL
             ("<commands_response"
              " status=\"" STATUS_OK "\" status_text=\"" STATUS_OK_TEXT "\">");
            set_client_state (CLIENT_AUTHENTIC_COMMANDS);
          }
        else if (strcasecmp ("CREATE_AGENT", element_name) == 0)
          {
            openvas_append_string (&create_agent_data->comment, "");
            openvas_append_string (&create_agent_data->name, "");
            openvas_append_string (&create_agent_data->installer, "");
            openvas_append_string (&create_agent_data->installer_filename, "");
            openvas_append_string (&create_agent_data->installer_signature, "");
            openvas_append_string (&create_agent_data->howto_install, "");
            openvas_append_string (&create_agent_data->howto_use, "");
            set_client_state (CLIENT_CREATE_AGENT);
          }
        else if (strcasecmp ("CREATE_CONFIG", element_name) == 0)
          {
            openvas_append_string (&create_config_data->comment, "");
            openvas_append_string (&create_config_data->name, "");
            set_client_state (CLIENT_CREATE_CONFIG);
          }
        else if (strcasecmp ("CREATE_ESCALATOR", element_name) == 0)
          {
            create_escalator_data->condition_data = make_array ();
            create_escalator_data->event_data = make_array ();
            create_escalator_data->method_data = make_array ();

            openvas_append_string (&create_escalator_data->part_data, "");
            openvas_append_string (&create_escalator_data->part_name, "");
            openvas_append_string (&create_escalator_data->comment, "");
            openvas_append_string (&create_escalator_data->name, "");
            openvas_append_string (&create_escalator_data->condition, "");
            openvas_append_string (&create_escalator_data->method, "");
            openvas_append_string (&create_escalator_data->event, "");

            set_client_state (CLIENT_CREATE_ESCALATOR);
          }
        else if (strcasecmp ("CREATE_LSC_CREDENTIAL", element_name) == 0)
          {
            openvas_append_string (&create_lsc_credential_data->comment, "");
            openvas_append_string (&create_lsc_credential_data->login, "");
            openvas_append_string (&create_lsc_credential_data->name, "");
            set_client_state (CLIENT_CREATE_LSC_CREDENTIAL);
          }
        else if (strcasecmp ("CREATE_NOTE", element_name) == 0)
          set_client_state (CLIENT_CREATE_NOTE);
        else if (strcasecmp ("CREATE_OVERRIDE", element_name) == 0)
          set_client_state (CLIENT_CREATE_OVERRIDE);
        else if (strcasecmp ("CREATE_REPORT_FORMAT", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_FORMAT);
        else if (strcasecmp ("CREATE_SLAVE", element_name) == 0)
          {
            openvas_append_string (&create_slave_data->comment, "");
            openvas_append_string (&create_slave_data->password, "");
            set_client_state (CLIENT_CREATE_SLAVE);
          }
        else if (strcasecmp ("CREATE_SCHEDULE", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE);
        else if (strcasecmp ("CREATE_TARGET", element_name) == 0)
          {
            openvas_append_string (&create_target_data->comment, "");
            openvas_append_string (&create_target_data->name, "");
            openvas_append_string (&create_target_data->hosts, "");
            set_client_state (CLIENT_CREATE_TARGET);
          }
        else if (strcasecmp ("CREATE_TASK", element_name) == 0)
          {
            create_task_data->task = make_task (NULL, 0, NULL);
            set_client_state (CLIENT_CREATE_TASK);
          }
        else if (strcasecmp ("DELETE_AGENT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values,
                              "agent_id", &delete_agent_data->agent_id);
            set_client_state (CLIENT_DELETE_AGENT);
          }
        else if (strcasecmp ("DELETE_CONFIG", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values,
                              "config_id", &delete_config_data->config_id);
            set_client_state (CLIENT_DELETE_CONFIG);
          }
        else if (strcasecmp ("DELETE_ESCALATOR", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values,
                              "escalator_id",
                              &delete_escalator_data->escalator_id);
            set_client_state (CLIENT_DELETE_ESCALATOR);
          }
        else if (strcasecmp ("DELETE_LSC_CREDENTIAL", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values,
                              "lsc_credential_id",
                              &delete_lsc_credential_data->lsc_credential_id);
            set_client_state (CLIENT_DELETE_LSC_CREDENTIAL);
          }
        else if (strcasecmp ("DELETE_NOTE", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "note_id",
                              &delete_note_data->note_id);
            set_client_state (CLIENT_DELETE_NOTE);
          }
        else if (strcasecmp ("DELETE_OVERRIDE", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "override_id",
                              &delete_override_data->override_id);
            set_client_state (CLIENT_DELETE_OVERRIDE);
          }
        else if (strcasecmp ("DELETE_REPORT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "report_id",
                              &delete_report_data->report_id);
            set_client_state (CLIENT_DELETE_REPORT);
          }
        else if (strcasecmp ("DELETE_REPORT_FORMAT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "report_format_id",
                              &delete_report_format_data->report_format_id);
            set_client_state (CLIENT_DELETE_REPORT_FORMAT);
          }
        else if (strcasecmp ("DELETE_SCHEDULE", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "schedule_id",
                              &delete_schedule_data->schedule_id);
            set_client_state (CLIENT_DELETE_SCHEDULE);
          }
        else if (strcasecmp ("DELETE_SLAVE", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "slave_id",
                              &delete_slave_data->slave_id);
            set_client_state (CLIENT_DELETE_SLAVE);
          }
        else if (strcasecmp ("DELETE_TARGET", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "target_id",
                              &delete_target_data->target_id);
            set_client_state (CLIENT_DELETE_TARGET);
          }
        else if (strcasecmp ("DELETE_TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "task_id",
                              &delete_task_data->task_id);
            set_client_state (CLIENT_DELETE_TASK);
          }
        else if (strcasecmp ("GET_AGENTS", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "agent_id",
                              &get_agents_data->agent_id);
            append_attribute (attribute_names, attribute_values, "format",
                              &get_agents_data->format);
            append_attribute (attribute_names, attribute_values, "sort_field",
                              &get_agents_data->sort_field);
            if (find_attribute (attribute_names, attribute_values,
                                "sort_order", &attribute))
              get_agents_data->sort_order = strcmp (attribute, "descending");
            else
              get_agents_data->sort_order = 1;
            set_client_state (CLIENT_GET_AGENTS);
          }
#if 0
        else if (strcasecmp ("GET_CERTIFICATES", element_name) == 0)
          set_client_state (CLIENT_GET_CERTIFICATES);
#endif
        else if (strcasecmp ("GET_CONFIGS", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "config_id",
                              &get_configs_data->config_id);
            if (find_attribute (attribute_names, attribute_values,
                                "families", &attribute))
              get_configs_data->families = atoi (attribute);
            else
              get_configs_data->families = 0;
            append_attribute (attribute_names, attribute_values, "sort_field",
                              &get_configs_data->sort_field);
            if (find_attribute (attribute_names, attribute_values,
                                "sort_order", &attribute))
              get_configs_data->sort_order = strcmp (attribute, "descending");
            else
              get_configs_data->sort_order = 1;
            if (find_attribute (attribute_names, attribute_values,
                                "preferences", &attribute))
              get_configs_data->preferences = atoi (attribute);
            else
              get_configs_data->preferences = 0;
            if (find_attribute (attribute_names, attribute_values,
                                "export", &attribute))
              get_configs_data->export = atoi (attribute);
            else
              get_configs_data->export = 0;
            set_client_state (CLIENT_GET_CONFIGS);
          }
        else if (strcasecmp ("GET_DEPENDENCIES", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "nvt_oid",
                              &get_dependencies_data->nvt_oid);
            set_client_state (CLIENT_GET_DEPENDENCIES);
          }
        else if (strcasecmp ("GET_ESCALATORS", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values,
                              "escalator_id",
                              &get_escalators_data->escalator_id);
            append_attribute (attribute_names, attribute_values, "sort_field",
                              &get_escalators_data->sort_field);
            if (find_attribute (attribute_names, attribute_values,
                                "sort_order", &attribute))
              get_escalators_data->sort_order = strcmp (attribute,
                                                        "descending");
            else
              get_escalators_data->sort_order = 1;
            set_client_state (CLIENT_GET_ESCALATORS);
          }
        else if (strcasecmp ("GET_LSC_CREDENTIALS", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values,
                              "lsc_credential_id",
                              &get_lsc_credentials_data->lsc_credential_id);
            append_attribute (attribute_names, attribute_values, "format",
                              &get_lsc_credentials_data->format);
            append_attribute (attribute_names, attribute_values, "sort_field",
                              &get_lsc_credentials_data->sort_field);
            if (find_attribute (attribute_names, attribute_values,
                                "sort_order", &attribute))
              get_lsc_credentials_data->sort_order = strcmp (attribute,
                                                             "descending");
            else
              get_lsc_credentials_data->sort_order = 1;
            set_client_state (CLIENT_GET_LSC_CREDENTIALS);
          }
        else if (strcasecmp ("GET_NOTES", element_name) == 0)
          {
            const gchar* attribute;

            append_attribute (attribute_names, attribute_values, "note_id",
                              &get_notes_data->note_id);

            append_attribute (attribute_names, attribute_values, "nvt_oid",
                              &get_notes_data->nvt_oid);

            append_attribute (attribute_names, attribute_values, "task_id",
                              &get_notes_data->task_id);

            if (find_attribute (attribute_names, attribute_values,
                                "details", &attribute))
              get_notes_data->details = strcmp (attribute, "0");
            else
              get_notes_data->details = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "result", &attribute))
              get_notes_data->result = strcmp (attribute, "0");
            else
              get_notes_data->result = 0;

            append_attribute (attribute_names, attribute_values, "sort_field",
                              &get_notes_data->sort_field);

            if (find_attribute (attribute_names, attribute_values,
                                "sort_order", &attribute))
              get_notes_data->sort_order = strcmp (attribute, "descending");
            else
              get_notes_data->sort_order = 1;

            set_client_state (CLIENT_GET_NOTES);
          }
        else if (strcasecmp ("GET_NVT_FEED_CHECKSUM", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "algorithm",
                              &get_nvt_feed_checksum_data->algorithm);
            set_client_state (CLIENT_GET_NVT_FEED_CHECKSUM);
          }
        else if (strcasecmp ("GET_NVTS", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "nvt_oid",
                              &get_nvts_data->nvt_oid);
            append_attribute (attribute_names, attribute_values, "config_id",
                              &get_nvts_data->config_id);
            if (find_attribute (attribute_names, attribute_values,
                                "details", &attribute))
              get_nvts_data->details = strcmp (attribute, "0");
            else
              get_nvts_data->details = 0;
            append_attribute (attribute_names, attribute_values, "family",
                              &get_nvts_data->family);
            if (find_attribute (attribute_names, attribute_values,
                                "preferences", &attribute))
              get_nvts_data->preferences = strcmp (attribute, "0");
            else
              get_nvts_data->preferences = 0;
            if (find_attribute (attribute_names, attribute_values,
                                "preference_count", &attribute))
              get_nvts_data->preference_count = strcmp (attribute, "0");
            else
              get_nvts_data->preference_count = 0;
            if (find_attribute (attribute_names, attribute_values,
                                "timeout", &attribute))
              get_nvts_data->timeout = strcmp (attribute, "0");
            else
              get_nvts_data->timeout = 0;
            append_attribute (attribute_names, attribute_values, "sort_field",
                              &get_nvts_data->sort_field);
            if (find_attribute (attribute_names, attribute_values,
                                "sort_order", &attribute))
              get_nvts_data->sort_order = strcmp (attribute,
                                                         "descending");
            else
              get_nvts_data->sort_order = 1;
            set_client_state (CLIENT_GET_NVTS);
          }
        else if (strcasecmp ("GET_NVT_FAMILIES", element_name) == 0)
          {
            const gchar* attribute;
            if (find_attribute (attribute_names, attribute_values,
                                "sort_order", &attribute))
              get_nvt_families_data->sort_order = strcmp (attribute,
                                                          "descending");
            else
              get_nvt_families_data->sort_order = 1;
            set_client_state (CLIENT_GET_NVT_FAMILIES);
          }
        else if (strcasecmp ("GET_OVERRIDES", element_name) == 0)
          {
            const gchar* attribute;

            append_attribute (attribute_names, attribute_values, "override_id",
                              &get_overrides_data->override_id);

            append_attribute (attribute_names, attribute_values, "nvt_oid",
                              &get_overrides_data->nvt_oid);

            append_attribute (attribute_names, attribute_values, "task_id",
                              &get_overrides_data->task_id);

            if (find_attribute (attribute_names, attribute_values,
                                "details", &attribute))
              get_overrides_data->details = strcmp (attribute, "0");
            else
              get_overrides_data->details = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "result", &attribute))
              get_overrides_data->result = strcmp (attribute, "0");
            else
              get_overrides_data->result = 0;

            append_attribute (attribute_names, attribute_values, "sort_field",
                              &get_overrides_data->sort_field);

            if (find_attribute (attribute_names, attribute_values,
                                "sort_order", &attribute))
              get_overrides_data->sort_order = strcmp (attribute, "descending");
            else
              get_overrides_data->sort_order = 1;

            set_client_state (CLIENT_GET_OVERRIDES);
          }
        else if (strcasecmp ("GET_PREFERENCES", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "nvt_oid",
                              &get_preferences_data->nvt_oid);
            append_attribute (attribute_names, attribute_values, "config_id",
                              &get_preferences_data->config_id);
            append_attribute (attribute_names, attribute_values, "preference",
                              &get_preferences_data->preference);
            set_client_state (CLIENT_GET_PREFERENCES);
          }
        else if (strcasecmp ("GET_REPORTS", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "report_id",
                              &get_reports_data->report_id);

            append_attribute (attribute_names, attribute_values, "format_id",
                              &get_reports_data->format_id);

            if (find_attribute (attribute_names, attribute_values,
                                "first_result", &attribute))
              /* Subtract 1 to switch from 1 to 0 indexing. */
              get_reports_data->first_result = atoi (attribute) - 1;
            else
              get_reports_data->first_result = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "max_results", &attribute))
              get_reports_data->max_results = atoi (attribute);
            else
              get_reports_data->max_results = -1;

            append_attribute (attribute_names, attribute_values, "sort_field",
                              &get_reports_data->sort_field);

            if (find_attribute (attribute_names, attribute_values,
                                "sort_order", &attribute))
              get_reports_data->sort_order = strcmp (attribute, "descending");
            else
              {
                if (get_reports_data->sort_field == NULL
                    || (strcmp (get_reports_data->sort_field, "type") == 0))
                  /* Normally it makes more sense to order type descending. */
                  get_reports_data->sort_order = 0;
                else
                  get_reports_data->sort_order = 1;
              }

            append_attribute (attribute_names, attribute_values, "levels",
                              &get_reports_data->levels);

            append_attribute (attribute_names, attribute_values,
                              "search_phrase",
                              &get_reports_data->search_phrase);

            if (find_attribute (attribute_names, attribute_values,
                                "notes", &attribute))
              get_reports_data->notes = strcmp (attribute, "0");
            else
              get_reports_data->notes = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "notes_details", &attribute))
              get_reports_data->notes_details = strcmp (attribute, "0");
            else
              get_reports_data->notes_details = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "overrides", &attribute))
              get_reports_data->overrides = strcmp (attribute, "0");
            else
              get_reports_data->overrides = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "overrides_details", &attribute))
              get_reports_data->overrides_details = strcmp (attribute, "0");
            else
              get_reports_data->overrides_details = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "apply_overrides", &attribute))
              get_results_data->apply_overrides = strcmp (attribute, "0");
            else
              get_results_data->apply_overrides = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "result_hosts_only", &attribute))
              get_reports_data->result_hosts_only = strcmp (attribute, "0");
            else
              get_reports_data->result_hosts_only = 1;

            append_attribute (attribute_names, attribute_values,
                              "min_cvss_base",
                              &get_reports_data->min_cvss_base);

            set_client_state (CLIENT_GET_REPORTS);
          }
        else if (strcasecmp ("GET_REPORT_FORMATS", element_name) == 0)
          {
            const gchar* attribute;

            append_attribute (attribute_names,
                              attribute_values,
                              "report_format_id",
                              &get_report_formats_data->report_format_id);

            if (find_attribute (attribute_names, attribute_values,
                                "export", &attribute))
              get_report_formats_data->export = strcmp (attribute, "0");
            else
              get_report_formats_data->export = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "params", &attribute))
              get_report_formats_data->params = strcmp (attribute, "0");
            else
              get_report_formats_data->params = 0;

            append_attribute (attribute_names, attribute_values, "sort_field",
                              &get_report_formats_data->sort_field);

            if (find_attribute (attribute_names, attribute_values,
                                "sort_order", &attribute))
              get_report_formats_data->sort_order = strcmp (attribute, "descending");
            else
              get_report_formats_data->sort_order = 1;

            set_client_state (CLIENT_GET_REPORT_FORMATS);
          }
        else if (strcasecmp ("GET_RESULTS", element_name) == 0)
          {
            const gchar* attribute;

            append_attribute (attribute_names, attribute_values, "result_id",
                              &get_results_data->result_id);

            append_attribute (attribute_names, attribute_values, "task_id",
                              &get_results_data->task_id);

            if (find_attribute (attribute_names, attribute_values,
                                "notes", &attribute))
              get_results_data->notes = strcmp (attribute, "0");
            else
              get_results_data->notes = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "notes_details", &attribute))
              get_results_data->notes_details = strcmp (attribute, "0");
            else
              get_results_data->notes_details = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "overrides", &attribute))
              get_results_data->overrides = strcmp (attribute, "0");
            else
              get_results_data->overrides = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "overrides_details", &attribute))
              get_results_data->overrides_details = strcmp (attribute, "0");
            else
              get_results_data->overrides_details = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "apply_overrides", &attribute))
              get_results_data->apply_overrides = strcmp (attribute, "0");
            else
              get_results_data->apply_overrides = 0;

            set_client_state (CLIENT_GET_RESULTS);
          }
        else if (strcasecmp ("GET_SCHEDULES", element_name) == 0)
          {
            const gchar* attribute;

            append_attribute (attribute_names, attribute_values,
                              "schedule_id",
                              &get_schedules_data->schedule_id);

            if (find_attribute (attribute_names, attribute_values,
                                "details", &attribute))
              get_schedules_data->details = strcmp (attribute, "0");
            else
              get_schedules_data->details = 0;

            append_attribute (attribute_names, attribute_values, "sort_field",
                              &get_schedules_data->sort_field);

            if (find_attribute (attribute_names, attribute_values,
                                "sort_order", &attribute))
              get_schedules_data->sort_order = strcmp (attribute, "descending");
            else
              get_schedules_data->sort_order = 1;

            set_client_state (CLIENT_GET_SCHEDULES);
          }
        else if (strcasecmp ("GET_SLAVES", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "slave_id",
                              &get_slaves_data->slave_id);
            if (find_attribute (attribute_names, attribute_values,
                                "tasks", &attribute))
              get_slaves_data->tasks = strcmp (attribute, "0");
            else
              get_slaves_data->tasks = 0;
            append_attribute (attribute_names, attribute_values, "sort_field",
                              &get_slaves_data->sort_field);
            if (find_attribute (attribute_names, attribute_values,
                                "sort_order", &attribute))
              get_slaves_data->sort_order = strcmp (attribute, "descending");
            else
              get_slaves_data->sort_order = 1;
            set_client_state (CLIENT_GET_SLAVES);
          }
        else if (strcasecmp ("GET_TARGET_LOCATORS", element_name) == 0)
          {
            set_client_state (CLIENT_GET_TARGET_LOCATORS);
          }
        else if (strcasecmp ("GET_SYSTEM_REPORTS", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "name",
                              &get_system_reports_data->name);
            append_attribute (attribute_names, attribute_values, "duration",
                              &get_system_reports_data->duration);
            if (find_attribute (attribute_names, attribute_values,
                                "brief", &attribute))
              get_system_reports_data->brief = strcmp (attribute, "0");
            else
              get_system_reports_data->brief = 0;
            set_client_state (CLIENT_GET_SYSTEM_REPORTS);
          }
        else if (strcasecmp ("GET_TARGETS", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "target_id",
                              &get_targets_data->target_id);
            if (find_attribute (attribute_names, attribute_values,
                                "tasks", &attribute))
              get_targets_data->tasks = strcmp (attribute, "0");
            else
              get_targets_data->tasks = 0;
            append_attribute (attribute_names, attribute_values, "sort_field",
                              &get_targets_data->sort_field);
            if (find_attribute (attribute_names, attribute_values,
                                "sort_order", &attribute))
              get_targets_data->sort_order = strcmp (attribute, "descending");
            else
              get_targets_data->sort_order = 1;
            set_client_state (CLIENT_GET_TARGETS);
          }
        else if (strcasecmp ("GET_TASKS", element_name) == 0)
          {
            const gchar* attribute;

            append_attribute (attribute_names, attribute_values, "task_id",
                              &get_tasks_data->task_id);

            if (find_attribute (attribute_names, attribute_values,
                                "rcfile", &attribute))
              get_tasks_data->rcfile = atoi (attribute);
            else
              get_tasks_data->rcfile = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "details", &attribute))
              get_tasks_data->details = strcmp (attribute, "0");
            else
              get_tasks_data->details = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "apply_overrides", &attribute))
              get_tasks_data->apply_overrides = strcmp (attribute, "0");
            else
              get_tasks_data->apply_overrides = 0;

            append_attribute (attribute_names, attribute_values, "sort_field",
                              &get_tasks_data->sort_field);

            if (find_attribute (attribute_names, attribute_values,
                                "sort_order", &attribute))
              get_tasks_data->sort_order = strcmp (attribute, "descending");
            else
              get_tasks_data->sort_order = 1;

            set_client_state (CLIENT_GET_TASKS);
          }
        else if (strcasecmp ("GET_VERSION", element_name) == 0)
          set_client_state (CLIENT_GET_VERSION_AUTHENTIC);
        else if (strcasecmp ("HELP", element_name) == 0)
          set_client_state (CLIENT_HELP);
        else if (strcasecmp ("MODIFY_CONFIG", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "config_id",
                              &modify_config_data->config_id);
            set_client_state (CLIENT_MODIFY_CONFIG);
          }
        else if (strcasecmp ("MODIFY_NOTE", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "note_id",
                              &modify_note_data->note_id);
            set_client_state (CLIENT_MODIFY_NOTE);
          }
        else if (strcasecmp ("MODIFY_OVERRIDE", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "override_id",
                              &modify_override_data->override_id);
            set_client_state (CLIENT_MODIFY_OVERRIDE);
          }
        else if (strcasecmp ("MODIFY_REPORT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "report_id",
                              &modify_report_data->report_id);
            set_client_state (CLIENT_MODIFY_REPORT);
          }
        else if (strcasecmp ("MODIFY_REPORT_FORMAT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values,
                              "report_format_id",
                              &modify_report_format_data->report_format_id);
            set_client_state (CLIENT_MODIFY_REPORT_FORMAT);
          }
        else if (strcasecmp ("MODIFY_TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "task_id",
                              &modify_task_data->task_id);
            set_client_state (CLIENT_MODIFY_TASK);
          }
        else if (strcasecmp ("PAUSE_TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "task_id",
                              &pause_task_data->task_id);
            set_client_state (CLIENT_PAUSE_TASK);
          }
        else if (strcasecmp ("RESUME_OR_START_TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "task_id",
                              &resume_or_start_task_data->task_id);
            set_client_state (CLIENT_RESUME_OR_START_TASK);
          }
        else if (strcasecmp ("RESUME_PAUSED_TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "task_id",
                              &resume_paused_task_data->task_id);
            set_client_state (CLIENT_RESUME_PAUSED_TASK);
          }
        else if (strcasecmp ("RESUME_STOPPED_TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "task_id",
                              &resume_paused_task_data->task_id);
            set_client_state (CLIENT_RESUME_STOPPED_TASK);
          }
        else if (strcasecmp ("START_TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "task_id",
                              &start_task_data->task_id);
            set_client_state (CLIENT_START_TASK);
          }
        else if (strcasecmp ("STOP_TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "task_id",
                              &stop_task_data->task_id);
            set_client_state (CLIENT_STOP_TASK);
          }
        else if (strcasecmp ("TEST_ESCALATOR", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values,
                              "escalator_id",
                              &test_escalator_data->escalator_id);
            set_client_state (CLIENT_TEST_ESCALATOR);
          }
        else if (strcasecmp ("VERIFY_AGENT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "agent_id",
                              &verify_agent_data->agent_id);
            set_client_state (CLIENT_VERIFY_AGENT);
          }
        else if (strcasecmp ("VERIFY_REPORT_FORMAT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "report_format_id",
                              &verify_report_format_data->report_format_id);
            set_client_state (CLIENT_VERIFY_REPORT_FORMAT);
          }
        else
          {
            if (send_to_client (XML_ERROR_SYNTAX ("omp", "Bogus command name"),
                                write_to_client,
                                write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_AUTHENTICATE:
        if (strcasecmp ("CREDENTIALS", element_name) == 0)
          {
            /* Init, so it's the empty string when the entity is empty. */
            append_to_credentials_password (&current_credentials, "", 0);
            set_client_state (CLIENT_AUTHENTICATE_CREDENTIALS);
          }
        else
          {
            if (send_element_error_to_client ("authenticate", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            free_credentials (&current_credentials);
            set_client_state (CLIENT_TOP);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;
      case CLIENT_AUTHENTICATE_CREDENTIALS:
        if (strcasecmp ("USERNAME", element_name) == 0)
          set_client_state (CLIENT_AUTHENTICATE_CREDENTIALS_USERNAME);
        else if (strcasecmp ("PASSWORD", element_name) == 0)
          set_client_state (CLIENT_AUTHENTICATE_CREDENTIALS_PASSWORD);
        else
          {
            if (send_element_error_to_client ("authenticate", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            free_credentials (&current_credentials);
            set_client_state (CLIENT_TOP);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_SCHEDULE:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_COMMENT);
        else if (strcasecmp ("DURATION", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_DURATION);
        else if (strcasecmp ("FIRST_TIME", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_FIRST_TIME);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_NAME);
        else if (strcasecmp ("PERIOD", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_PERIOD);
        else
          {
            if (send_element_error_to_client ("create_schedule", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_SCHEDULE_FIRST_TIME:
        if (strcasecmp ("DAY_OF_MONTH", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_FIRST_TIME_DAY_OF_MONTH);
        else if (strcasecmp ("HOUR", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_FIRST_TIME_HOUR);
        else if (strcasecmp ("MINUTE", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_FIRST_TIME_MINUTE);
        else if (strcasecmp ("MONTH", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_FIRST_TIME_MONTH);
        else if (strcasecmp ("YEAR", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_FIRST_TIME_YEAR);
        else
          {
            if (send_element_error_to_client ("create_schedule", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_SCHEDULE_DURATION:
        if (strcasecmp ("UNIT", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_DURATION_UNIT);
        else
          {
            if (send_element_error_to_client ("create_schedule", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_SCHEDULE_PERIOD:
        if (strcasecmp ("UNIT", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_PERIOD_UNIT);
        else
          {
            if (send_element_error_to_client ("create_schedule", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_SCHEDULE_COMMENT:
      case CLIENT_CREATE_SCHEDULE_NAME:
      case CLIENT_CREATE_SCHEDULE_FIRST_TIME_DAY_OF_MONTH:
      case CLIENT_CREATE_SCHEDULE_FIRST_TIME_HOUR:
      case CLIENT_CREATE_SCHEDULE_FIRST_TIME_MINUTE:
      case CLIENT_CREATE_SCHEDULE_FIRST_TIME_MONTH:
      case CLIENT_CREATE_SCHEDULE_FIRST_TIME_YEAR:
      case CLIENT_CREATE_SCHEDULE_DURATION_UNIT:
      case CLIENT_CREATE_SCHEDULE_PERIOD_UNIT:
        if (send_element_error_to_client ("create_schedule", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_DELETE_AGENT:
          {
            if (send_element_error_to_client ("delete_agent",
                                              element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_DELETE_CONFIG:
        if (send_element_error_to_client ("delete_config", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_DELETE_ESCALATOR:
        if (send_element_error_to_client ("delete_escalator", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_DELETE_LSC_CREDENTIAL:
        if (send_element_error_to_client ("delete_lsc_credential",
                                          element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_DELETE_NOTE:
        if (send_element_error_to_client ("delete_note", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_DELETE_OVERRIDE:
        if (send_element_error_to_client ("delete_override", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_DELETE_REPORT:
        if (send_element_error_to_client ("delete_report", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_DELETE_REPORT_FORMAT:
        if (send_element_error_to_client ("delete_report_format", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_DELETE_SCHEDULE:
        if (send_element_error_to_client ("delete_schedule", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_DELETE_SLAVE:
        if (send_element_error_to_client ("delete_slave", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_DELETE_TARGET:
        if (send_element_error_to_client ("delete_target", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_DELETE_TASK:
        if (send_element_error_to_client ("delete_task", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_GET_AGENTS:
          {
            if (send_element_error_to_client ("get_agents",
                                              element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

#if 0
      case CLIENT_GET_CERTIFICATES:
          {
            if (send_element_error_to_client ("get_certificates", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;
#endif

      case CLIENT_GET_CONFIGS:
          {
            if (send_element_error_to_client ("get_configs", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_GET_DEPENDENCIES:
          {
            if (send_element_error_to_client ("get_dependencies", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_GET_ESCALATORS:
          {
            if (send_element_error_to_client ("get_escalators", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_GET_LSC_CREDENTIALS:
          {
            if (send_element_error_to_client ("get_lsc_credentials",
                                              element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_GET_NOTES:
          {
            if (send_element_error_to_client ("get_notes", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_GET_NVT_FEED_CHECKSUM:
          {
            if (send_element_error_to_client ("get_nvt_feed_checksum",
                                              element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_GET_NVTS:
        if (send_element_error_to_client ("get_nvts", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_GET_NVT_FAMILIES:
        if (send_element_error_to_client ("get_nvt_families", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_GET_OVERRIDES:
          {
            if (send_element_error_to_client ("get_overrides", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_GET_PREFERENCES:
          {
            if (send_element_error_to_client ("get_preferences", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_GET_REPORTS:
        if (send_element_error_to_client ("get_reports", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_GET_REPORT_FORMATS:
          {
            if (send_element_error_to_client ("get_report_formats",
                                              element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_GET_RESULTS:
        if (send_element_error_to_client ("get_results", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_GET_SCHEDULES:
          {
            if (send_element_error_to_client ("get_schedules", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_GET_SLAVES:
          {
            if (send_element_error_to_client ("get_slaves", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_GET_SYSTEM_REPORTS:
          {
            if (send_element_error_to_client ("get_system_reports", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_GET_TARGETS:
          {
            if (send_element_error_to_client ("get_targets", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;
      case CLIENT_GET_TARGET_LOCATORS:
        {
          if (send_element_error_to_client ("get_target_locators", element_name,
                                            write_to_client,
                                            write_to_client_data))
            {
              error_send_to_client (error);
              return;
            }
          set_client_state (CLIENT_AUTHENTIC);
          g_set_error (error,
                      G_MARKUP_ERROR,
                      G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                      "Error");
          break;
        }

      case CLIENT_GET_TASKS:
        if (send_element_error_to_client ("get_tasks", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_HELP:
        {
          if (send_element_error_to_client ("help", element_name,
                                            write_to_client,
                                            write_to_client_data))
            {
              error_send_to_client (error);
              return;
            }
          set_client_state (CLIENT_AUTHENTIC);
          g_set_error (error,
                       G_MARKUP_ERROR,
                       G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                       "Error");
        }
        break;

      case CLIENT_MODIFY_CONFIG:
        if (strcasecmp ("FAMILY_SELECTION", element_name) == 0)
          {
            modify_config_data->families_growing_all = make_array ();
            modify_config_data->families_static_all = make_array ();
            modify_config_data->families_growing_empty = make_array ();
            /* For GROWING entity, in case missing. */
            modify_config_data->family_selection_growing = 0;
            set_client_state (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION);
          }
        else if (strcasecmp ("NVT_SELECTION", element_name) == 0)
          {
            modify_config_data->nvt_selection = make_array ();
            set_client_state (CLIENT_MODIFY_CONFIG_NVT_SELECTION);
          }
        else if (strcasecmp ("PREFERENCE", element_name) == 0)
          set_client_state (CLIENT_MODIFY_CONFIG_PREFERENCE);
        else
          {
            if (send_element_error_to_client ("modify_config", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_MODIFY_CONFIG_NVT_SELECTION:
        if (strcasecmp ("FAMILY", element_name) == 0)
          set_client_state (CLIENT_MODIFY_CONFIG_NVT_SELECTION_FAMILY);
        else if (strcasecmp ("NVT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "oid",
                              &modify_config_data->nvt_selection_nvt_oid);
            set_client_state (CLIENT_MODIFY_CONFIG_NVT_SELECTION_NVT);
          }
        else
          {
            if (send_element_error_to_client ("modify_config", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_MODIFY_CONFIG_FAMILY_SELECTION:
        if (strcasecmp ("FAMILY", element_name) == 0)
          {
            /* For ALL entity, in case missing. */
            modify_config_data->family_selection_family_all = 0;
            /* For GROWING entity, in case missing. */
            modify_config_data->family_selection_family_growing = 0;
            set_client_state (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY);
          }
        else if (strcasecmp ("GROWING", element_name) == 0)
          set_client_state (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_GROWING);
        else
          {
            if (send_element_error_to_client ("modify_config", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY:
        if (strcasecmp ("ALL", element_name) == 0)
          set_client_state
           (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_ALL);
        else if (strcasecmp ("GROWING", element_name) == 0)
          set_client_state
           (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_GROWING);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_NAME);
        else
          {
            if (send_element_error_to_client ("modify_config", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_MODIFY_CONFIG_PREFERENCE:
        if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_CONFIG_PREFERENCE_NAME);
        else if (strcasecmp ("NVT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "oid",
                              &modify_config_data->preference_nvt_oid);
            set_client_state (CLIENT_MODIFY_CONFIG_PREFERENCE_NVT);
          }
        else if (strcasecmp ("VALUE", element_name) == 0)
          set_client_state (CLIENT_MODIFY_CONFIG_PREFERENCE_VALUE);
        else
          {
            if (send_element_error_to_client ("modify_config", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_MODIFY_REPORT:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_REPORT_COMMENT);
        else
          {
            if (send_element_error_to_client ("modify_report", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_MODIFY_REPORT_FORMAT:
        if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_REPORT_FORMAT_NAME);
        else if (strcasecmp ("SUMMARY", element_name) == 0)
          set_client_state (CLIENT_MODIFY_REPORT_FORMAT_SUMMARY);
        else
          {
            if (send_element_error_to_client ("modify_report_format",
                                              element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_MODIFY_TASK:
        if (strcasecmp ("COMMENT", element_name) == 0)
          {
            openvas_append_string (&modify_task_data->comment, "");
            set_client_state (CLIENT_MODIFY_TASK_COMMENT);
          }
        else if (strcasecmp ("ESCALATOR", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &modify_task_data->escalator_id);
            set_client_state (CLIENT_MODIFY_TASK_ESCALATOR);
          }
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_TASK_NAME);
        else if (strcasecmp ("RCFILE", element_name) == 0)
          set_client_state (CLIENT_MODIFY_TASK_RCFILE);
        else if (strcasecmp ("SCHEDULE", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &modify_task_data->schedule_id);
            set_client_state (CLIENT_MODIFY_TASK_SCHEDULE);
          }
        else if (strcasecmp ("FILE", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "name",
                              &modify_task_data->file_name);
            if (find_attribute (attribute_names, attribute_values,
                                "action", &attribute))
              openvas_append_string (&modify_task_data->action, attribute);
            else
              openvas_append_string (&modify_task_data->action, "update");
            set_client_state (CLIENT_MODIFY_TASK_FILE);
          }
        else
          {
            if (send_element_error_to_client ("modify_task", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_AGENT:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_AGENT_COMMENT);
        else if (strcasecmp ("HOWTO_INSTALL", element_name) == 0)
          set_client_state (CLIENT_CREATE_AGENT_HOWTO_INSTALL);
        else if (strcasecmp ("HOWTO_USE", element_name) == 0)
          set_client_state (CLIENT_CREATE_AGENT_HOWTO_USE);
        else if (strcasecmp ("INSTALLER", element_name) == 0)
          set_client_state (CLIENT_CREATE_AGENT_INSTALLER);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_AGENT_NAME);
        else
          {
            if (send_element_error_to_client ("create_agent",
                                              element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;
      case CLIENT_CREATE_AGENT_INSTALLER:
        if (strcasecmp ("FILENAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_AGENT_INSTALLER_FILENAME);
        else if (strcasecmp ("SIGNATURE", element_name) == 0)
          set_client_state (CLIENT_CREATE_AGENT_INSTALLER_SIGNATURE);
        else
          {
            if (send_element_error_to_client ("create_agent",
                                              element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_CONFIG:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_CONFIG_COMMENT);
        else if (strcasecmp ("COPY", element_name) == 0)
          set_client_state (CLIENT_CREATE_CONFIG_COPY);
        else if (strcasecmp ("GET_CONFIGS_RESPONSE", element_name) == 0)
          set_client_state (CLIENT_C_C_GCR);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_CONFIG_NAME);
        else if (strcasecmp ("RCFILE", element_name) == 0)
          set_client_state (CLIENT_CREATE_CONFIG_RCFILE);
        else
          {
            if (send_element_error_to_client ("create_config", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_C_C_GCR:
        if (strcasecmp ("CONFIG", element_name) == 0)
          {
            /* Reset here in case there was a previous config element. */
            create_config_data_reset (create_config_data);
            set_client_state (CLIENT_C_C_GCR_CONFIG);
          }
        else
          {
            if (send_element_error_to_client ("create_config", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_C_C_GCR_CONFIG:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_C_C_GCR_CONFIG_COMMENT);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_C_C_GCR_CONFIG_NAME);
        else if (strcasecmp ("NVT_SELECTORS", element_name) == 0)
          {
            /* Reset array, in case there was a previous nvt_selectors element. */
            array_reset (&import_config_data->nvt_selectors);
            set_client_state (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS);
          }
        else if (strcasecmp ("PREFERENCES", element_name) == 0)
          {
            /* Reset array, in case there was a previous preferences element. */
            array_reset (&import_config_data->preferences);
            set_client_state (CLIENT_C_C_GCR_CONFIG_PREFERENCES);
          }
        else
          {
            if (send_element_error_to_client ("create_config", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS:
        if (strcasecmp ("NVT_SELECTOR", element_name) == 0)
          set_client_state (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR);
        else
          {
            if (send_element_error_to_client ("create_config", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR:
        if (strcasecmp ("INCLUDE", element_name) == 0)
          set_client_state
           (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_INCLUDE);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state
           (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_NAME);
        else if (strcasecmp ("TYPE", element_name) == 0)
          set_client_state
           (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_TYPE);
        else if (strcasecmp ("FAMILY_OR_NVT", element_name) == 0)
          set_client_state
           (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_FAMILY_OR_NVT);
        else
          {
            if (send_element_error_to_client ("create_config", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_C_C_GCR_CONFIG_PREFERENCES:
        if (strcasecmp ("PREFERENCE", element_name) == 0)
          {
            array_reset (&import_config_data->preference_alts);
            set_client_state (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE);
          }
        else
          {
            if (send_element_error_to_client ("create_config", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE:
        if (strcasecmp ("ALT", element_name) == 0)
          set_client_state
           (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_ALT);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state
           (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NAME);
        else if (strcasecmp ("NVT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "oid",
                              &import_config_data->preference_nvt_oid);
            set_client_state
             (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NVT);
          }
        else if (strcasecmp ("TYPE", element_name) == 0)
          set_client_state
           (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_TYPE);
        else if (strcasecmp ("VALUE", element_name) == 0)
          set_client_state
           (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_VALUE);
        else
          {
            if (send_element_error_to_client ("create_config", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NVT:
        if (strcasecmp ("NAME", element_name) == 0)
          set_client_state
           (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NVT_NAME);
        else
          {
            if (send_element_error_to_client ("create_config", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_C_C_GCR_CONFIG_COMMENT:
      case CLIENT_C_C_GCR_CONFIG_NAME:
      case CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_INCLUDE:
      case CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_NAME:
      case CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_TYPE:
      case CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_FAMILY_OR_NVT:
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_ALT:
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NAME:
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NVT_NAME:
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_TYPE:
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_VALUE:
        if (send_element_error_to_client ("create_config", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_CREATE_ESCALATOR:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_ESCALATOR_COMMENT);
        else if (strcasecmp ("CONDITION", element_name) == 0)
          set_client_state (CLIENT_CREATE_ESCALATOR_CONDITION);
        else if (strcasecmp ("EVENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_ESCALATOR_EVENT);
        else if (strcasecmp ("METHOD", element_name) == 0)
          set_client_state (CLIENT_CREATE_ESCALATOR_METHOD);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_ESCALATOR_NAME);
        else
          {
            if (send_element_error_to_client ("create_escalator", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_ESCALATOR_CONDITION:
        if (strcasecmp ("DATA", element_name) == 0)
          set_client_state (CLIENT_CREATE_ESCALATOR_CONDITION_DATA);
        else
          {
            if (send_element_error_to_client ("create_escalator", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_ESCALATOR_CONDITION_DATA:
        if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_ESCALATOR_CONDITION_DATA_NAME);
        else
          {
            if (send_element_error_to_client ("create_escalator", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_ESCALATOR_EVENT:
        if (strcasecmp ("DATA", element_name) == 0)
          set_client_state (CLIENT_CREATE_ESCALATOR_EVENT_DATA);
        else
          {
            if (send_element_error_to_client ("create_escalator", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_ESCALATOR_EVENT_DATA:
        if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_ESCALATOR_EVENT_DATA_NAME);
        else
          {
            if (send_element_error_to_client ("create_escalator", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_ESCALATOR_METHOD:
        if (strcasecmp ("DATA", element_name) == 0)
          set_client_state (CLIENT_CREATE_ESCALATOR_METHOD_DATA);
        else
          {
            if (send_element_error_to_client ("create_escalator", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_ESCALATOR_METHOD_DATA:
        if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_ESCALATOR_METHOD_DATA_NAME);
        else
          {
            if (send_element_error_to_client ("create_escalator", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_LSC_CREDENTIAL:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_LSC_CREDENTIAL_COMMENT);
        else if (strcasecmp ("LOGIN", element_name) == 0)
          set_client_state (CLIENT_CREATE_LSC_CREDENTIAL_LOGIN);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_LSC_CREDENTIAL_NAME);
        else if (strcasecmp ("PASSWORD", element_name) == 0)
          {
            openvas_append_string (&create_lsc_credential_data->password, "");
            set_client_state (CLIENT_CREATE_LSC_CREDENTIAL_PASSWORD);
          }
        else
          {
            if (send_element_error_to_client ("create_lsc_credential",
                                              element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_NOTE:
        if (strcasecmp ("HOSTS", element_name) == 0)
          set_client_state (CLIENT_CREATE_NOTE_HOSTS);
        else if (strcasecmp ("NVT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "oid",
                              &create_note_data->nvt_oid);
            set_client_state (CLIENT_CREATE_NOTE_NVT);
          }
        else if (strcasecmp ("PORT", element_name) == 0)
          set_client_state (CLIENT_CREATE_NOTE_PORT);
        else if (strcasecmp ("RESULT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_note_data->result_id);
            if (create_note_data->result_id
                && create_note_data->result_id[0] == '\0')
              {
                g_free (create_note_data->result_id);
                create_note_data->result_id = NULL;
              }
            set_client_state (CLIENT_CREATE_NOTE_RESULT);
          }
        else if (strcasecmp ("TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_note_data->task_id);
            if (create_note_data->task_id
                && create_note_data->task_id[0] == '\0')
              {
                g_free (create_note_data->task_id);
                create_note_data->task_id = NULL;
              }
            set_client_state (CLIENT_CREATE_NOTE_TASK);
          }
        else if (strcasecmp ("TEXT", element_name) == 0)
          set_client_state (CLIENT_CREATE_NOTE_TEXT);
        else if (strcasecmp ("THREAT", element_name) == 0)
          set_client_state (CLIENT_CREATE_NOTE_THREAT);
        else
          {
            if (send_element_error_to_client ("create_note", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_REPORT_FORMAT:
        if (strcasecmp ("GET_REPORT_FORMATS_RESPONSE", element_name) == 0)
          {
            create_report_format_data->import = 1;
            set_client_state (CLIENT_CRF_GRFR);
          }
        else
          {
            if (send_element_error_to_client ("create_report_format",
                                              element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CRF_GRFR:
        if (strcasecmp ("REPORT_FORMAT", element_name) == 0)
          {
            create_report_format_data->files = make_array ();
            create_report_format_data->params = make_array ();
            append_attribute (attribute_names, attribute_values, "id",
                              &create_report_format_data->id);
            set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT);
          }
        else
          {
            if (send_element_error_to_client ("create_report_format",
                                              element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CRF_GRFR_REPORT_FORMAT:
        if (strcasecmp ("CONTENT_TYPE", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_CONTENT_TYPE);
        else if (strcasecmp ("DESCRIPTION", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_DESCRIPTION);
        else if (strcasecmp ("EXTENSION", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_EXTENSION);
        else if (strcasecmp ("GLOBAL", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_GLOBAL);
        else if (strcasecmp ("FILE", element_name) == 0)
          {
            assert (create_report_format_data->file == NULL);
            assert (create_report_format_data->file_name == NULL);
            openvas_append_string (&create_report_format_data->file, "");
            append_attribute (attribute_names, attribute_values, "name",
                              &create_report_format_data->file_name);
            set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_FILE);
          }
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_NAME);
        else if (strcasecmp ("PARAM", element_name) == 0)
          {
            assert (create_report_format_data->param_name == NULL);
            assert (create_report_format_data->param_value == NULL);
            openvas_append_string (&create_report_format_data->param_name, "");
            openvas_append_string (&create_report_format_data->param_value, "");
            set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM);
          }
        else if (strcasecmp ("SIGNATURE", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_SIGNATURE);
        else if (strcasecmp ("SUMMARY", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_SUMMARY);
        else if (strcasecmp ("TRUST", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_TRUST);
        else
          {
            if (send_element_error_to_client ("create_report_format",
                                              element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CRF_GRFR_REPORT_FORMAT_CONTENT_TYPE:
        if (send_element_error_to_client ("create_report_format",
                                          element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_CRF_GRFR_REPORT_FORMAT_DESCRIPTION:
        if (send_element_error_to_client ("create_report_format",
                                          element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_CRF_GRFR_REPORT_FORMAT_EXTENSION:
        if (send_element_error_to_client ("create_report_format",
                                          element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_CRF_GRFR_REPORT_FORMAT_FILE:
        if (send_element_error_to_client ("create_report_format",
                                          element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_CRF_GRFR_REPORT_FORMAT_GLOBAL:
        if (send_element_error_to_client ("create_report_format",
                                          element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_CRF_GRFR_REPORT_FORMAT_NAME:
        if (send_element_error_to_client ("create_report_format",
                                          element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM:
        if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_NAME);
        else if (strcasecmp ("VALUE", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_VALUE);
        else
          {
            if (send_element_error_to_client ("create_report_format",
                                              element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_NAME:
      case CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_VALUE:
        if (send_element_error_to_client ("create_report_format",
                                          element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_CRF_GRFR_REPORT_FORMAT_SIGNATURE:
        if (send_element_error_to_client ("create_report_format",
                                          element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_CRF_GRFR_REPORT_FORMAT_SUMMARY:
        if (send_element_error_to_client ("create_report_format",
                                          element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_CRF_GRFR_REPORT_FORMAT_TRUST:
        if (send_element_error_to_client ("create_report_format",
                                          element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_CREATE_OVERRIDE:
        if (strcasecmp ("HOSTS", element_name) == 0)
          set_client_state (CLIENT_CREATE_OVERRIDE_HOSTS);
        else if (strcasecmp ("NEW_THREAT", element_name) == 0)
          set_client_state (CLIENT_CREATE_OVERRIDE_NEW_THREAT);
        else if (strcasecmp ("NVT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "oid",
                              &create_override_data->nvt_oid);
            set_client_state (CLIENT_CREATE_OVERRIDE_NVT);
          }
        else if (strcasecmp ("PORT", element_name) == 0)
          set_client_state (CLIENT_CREATE_OVERRIDE_PORT);
        else if (strcasecmp ("RESULT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_override_data->result_id);
            if (create_override_data->result_id
                && create_override_data->result_id[0] == '\0')
              {
                g_free (create_override_data->result_id);
                create_override_data->result_id = NULL;
              }
            set_client_state (CLIENT_CREATE_OVERRIDE_RESULT);
          }
        else if (strcasecmp ("TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_override_data->task_id);
            if (create_override_data->task_id
                && create_override_data->task_id[0] == '\0')
              {
                g_free (create_override_data->task_id);
                create_override_data->task_id = NULL;
              }
            set_client_state (CLIENT_CREATE_OVERRIDE_TASK);
          }
        else if (strcasecmp ("TEXT", element_name) == 0)
          set_client_state (CLIENT_CREATE_OVERRIDE_TEXT);
        else if (strcasecmp ("THREAT", element_name) == 0)
          set_client_state (CLIENT_CREATE_OVERRIDE_THREAT);
        else
          {
            if (send_element_error_to_client ("create_override", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_SLAVE:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_SLAVE_COMMENT);
        else if (strcasecmp ("HOST", element_name) == 0)
          set_client_state (CLIENT_CREATE_SLAVE_HOST);
        else if (strcasecmp ("LOGIN", element_name) == 0)
          set_client_state (CLIENT_CREATE_SLAVE_LOGIN);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_SLAVE_NAME);
        else if (strcasecmp ("PASSWORD", element_name) == 0)
          set_client_state (CLIENT_CREATE_SLAVE_PASSWORD);
        else if (strcasecmp ("PORT", element_name) == 0)
          set_client_state (CLIENT_CREATE_SLAVE_PORT);
        else
          {
            if (send_element_error_to_client ("create_slave", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_TARGET:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_TARGET_COMMENT);
        else if (strcasecmp ("HOSTS", element_name) == 0)
          set_client_state (CLIENT_CREATE_TARGET_HOSTS);
        else if (strcasecmp ("LSC_CREDENTIAL", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_target_data->lsc_credential_id);
            set_client_state (CLIENT_CREATE_TARGET_LSC_CREDENTIAL);
          }
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_TARGET_NAME);
        else if (strcasecmp ("TARGET_LOCATOR", element_name) == 0)
          set_client_state (CLIENT_CREATE_TARGET_TARGET_LOCATOR);
        else
          {
            if (send_element_error_to_client ("create_target", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_TARGET_TARGET_LOCATOR:
        if (strcasecmp ("PASSWORD", element_name) == 0)
          set_client_state (CLIENT_CREATE_TARGET_TARGET_LOCATOR_PASSWORD);
        else if (strcasecmp ("USERNAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_TARGET_TARGET_LOCATOR_USERNAME);
        else
          {
            if (send_element_error_to_client ("create_target", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_CREATE_TASK:
        if (strcasecmp ("RCFILE", element_name) == 0)
          {
            /* Initialise the task description. */
            if (create_task_data->task)
              add_task_description_line (create_task_data->task, "", 0);
            set_client_state (CLIENT_CREATE_TASK_RCFILE);
          }
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_TASK_NAME);
        else if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_TASK_COMMENT);
        else if (strcasecmp ("CONFIG", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_task_data->config_id);
            set_client_state (CLIENT_CREATE_TASK_CONFIG);
          }
        else if (strcasecmp ("ESCALATOR", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_task_data->escalator_id);
            set_client_state (CLIENT_CREATE_TASK_ESCALATOR);
          }
        else if (strcasecmp ("SCHEDULE", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_task_data->schedule_id);
            set_client_state (CLIENT_CREATE_TASK_SCHEDULE);
          }
        else if (strcasecmp ("SLAVE", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_task_data->slave_id);
            set_client_state (CLIENT_CREATE_TASK_SLAVE);
          }
        else if (strcasecmp ("TARGET", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_task_data->target_id);
            set_client_state (CLIENT_CREATE_TASK_TARGET);
          }
        else
          {
            if (send_element_error_to_client ("create_task", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_MODIFY_NOTE:
        if (strcasecmp ("HOSTS", element_name) == 0)
          set_client_state (CLIENT_MODIFY_NOTE_HOSTS);
        else if (strcasecmp ("PORT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_NOTE_PORT);
        else if (strcasecmp ("RESULT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &modify_note_data->result_id);
            if (modify_note_data->result_id
                && modify_note_data->result_id[0] == '\0')
              {
                g_free (modify_note_data->result_id);
                modify_note_data->result_id = NULL;
              }
            set_client_state (CLIENT_MODIFY_NOTE_RESULT);
          }
        else if (strcasecmp ("TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &modify_note_data->task_id);
            if (modify_note_data->task_id
                && modify_note_data->task_id[0] == '\0')
              {
                g_free (modify_note_data->task_id);
                modify_note_data->task_id = NULL;
              }
            set_client_state (CLIENT_MODIFY_NOTE_TASK);
          }
        else if (strcasecmp ("TEXT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_NOTE_TEXT);
        else if (strcasecmp ("THREAT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_NOTE_THREAT);
        else
          {
            if (send_element_error_to_client ("MODIFY_note", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_MODIFY_OVERRIDE:
        if (strcasecmp ("HOSTS", element_name) == 0)
          set_client_state (CLIENT_MODIFY_OVERRIDE_HOSTS);
        else if (strcasecmp ("NEW_THREAT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_OVERRIDE_NEW_THREAT);
        else if (strcasecmp ("PORT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_OVERRIDE_PORT);
        else if (strcasecmp ("RESULT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &modify_override_data->result_id);
            if (modify_override_data->result_id
                && modify_override_data->result_id[0] == '\0')
              {
                g_free (modify_override_data->result_id);
                modify_override_data->result_id = NULL;
              }
            set_client_state (CLIENT_MODIFY_OVERRIDE_RESULT);
          }
        else if (strcasecmp ("TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &modify_override_data->task_id);
            if (modify_override_data->task_id
                && modify_override_data->task_id[0] == '\0')
              {
                g_free (modify_override_data->task_id);
                modify_override_data->task_id = NULL;
              }
            set_client_state (CLIENT_MODIFY_OVERRIDE_TASK);
          }
        else if (strcasecmp ("TEXT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_OVERRIDE_TEXT);
        else if (strcasecmp ("THREAT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_OVERRIDE_THREAT);
        else
          {
            if (send_element_error_to_client ("modify_override", element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_TEST_ESCALATOR:
          {
            if (send_element_error_to_client ("test_escalator",
                                              element_name,
                                              write_to_client,
                                              write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            set_client_state (CLIENT_AUTHENTIC);
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_PAUSE_TASK:
        if (send_element_error_to_client ("pause_task", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_RESUME_OR_START_TASK:
        if (send_element_error_to_client ("resume_or_start_task", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_RESUME_PAUSED_TASK:
        if (send_element_error_to_client ("resume_paused_task", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_RESUME_STOPPED_TASK:
        if (send_element_error_to_client ("resume_stopped_task", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_START_TASK:
        if (send_element_error_to_client ("start_task", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_STOP_TASK:
        if (send_element_error_to_client ("stop_task", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_VERIFY_AGENT:
        if (send_element_error_to_client ("verify_agent", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      case CLIENT_VERIFY_REPORT_FORMAT:
        if (send_element_error_to_client ("verify_report_format", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;

      default:
        assert (0);
        /** @todo Respond with failure to client. */
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_PARSE,
                     "Manager programming error.");
        break;
    }

  return;
}

#if 0
/**
 * @brief Send XML for a certificate.
 *
 * @param[in]  cert_gp  The certificate.
 * @param[in]  dummy    Dummy variable, for certificate_find.
 *
 * @return 0 if out of space in to_client buffer, else 1.
 */
static gint
send_certificate (gpointer cert_gp, /*@unused@*/ gpointer dummy)
{
  certificate_t* cert = (certificate_t*) cert_gp;
  gchar* msg;

  const char* public_key = certificate_public_key (cert);
  const char* owner = certificate_owner (cert);
  gchar* owner_text = owner
                      ? g_markup_escape_text (owner, -1)
                      : g_strdup ("");

  msg = g_strdup_printf ("<certificate>"
                         "<fingerprint>%s</fingerprint>"
                         "<owner>%s</owner>"
                         "<trust_level>%s</trust_level>"
                         "<length>%zu</length>"
                         "<public_key>%s</public_key>"
                         "</certificate>",
                         certificate_fingerprint (cert),
                         owner_text,
                         certificate_trusted (cert) ? "trusted" : "notrust",
                         strlen (public_key),
                         public_key);
  g_free (owner_text);
  if (send_to_client (msg))
    {
      g_free (msg);
      return 0;
    }
  g_free (msg);
  return 1;
}
#endif

/**
 * @brief Send XML for a requirement of a plugin.
 *
 * @param[in]  element  The required plugin.
 * @param[in]  data     Array of two pointers: write_to_client and
 *                      write_to_client_data.
 *
 * @return 0 if out of space in to_client buffer, else 1.
 */
static gint
send_requirement (gconstpointer element, gconstpointer data)
{
  gboolean fail;
  gchar* text = g_markup_escape_text ((char*) element,
                                      strlen ((char*) element));
  char* oid = nvt_oid (text);
  gchar* msg = g_strdup_printf ("<nvt oid=\"%s\"><name>%s</name></nvt>",
                                oid ? oid : "",
                                text);
  int (*write_to_client) (void*) = (int (*) (void*)) *((void**)data);
  void* write_to_client_data = *(((void**)data) + 1);

  free (oid);
  g_free (text);

  fail = send_to_client (msg, write_to_client, write_to_client_data);
  g_free (msg);
  return fail ? 0 : 1;
}

/**
 * @brief Send XML for a plugin dependency.
 *
 * @param[in]  key    The dependency hashtable key.
 * @param[in]  value  The dependency hashtable value.
 * @param[in]  data   Array of two pointers: write_to_client and
 *                    write_to_client_data.
 *
 * @return TRUE if out of space in to_client buffer, else FALSE.
 */
static gboolean
send_dependency (gpointer key, gpointer value, gpointer data)
{
  gchar* key_text = g_markup_escape_text ((char*) key, strlen ((char*) key));
  char *oid = nvt_oid (key_text);
  gchar* msg = g_strdup_printf ("<nvt oid=\"%s\"><name>%s</name><requires>",
                                oid ? oid : "",
                                key_text);
  int (*write_to_client) (void*) = (int (*) (void*)) *((void**)data);
  void* write_to_client_data = *(((void**)data) + 1);

  g_free (oid);
  g_free (key_text);

  if (send_to_client (msg, write_to_client, write_to_client_data))
    {
      g_free (msg);
      return TRUE;
    }

  if (g_slist_find_custom ((GSList*) value, data, send_requirement))
    {
      g_free (msg);
      return TRUE;
    }

  if (send_to_client ("</requires></nvt>",
                      write_to_client,
                      write_to_client_data))
    {
      g_free (msg);
      return TRUE;
    }

  g_free (msg);
  return FALSE;
}

/**
 * @brief Define a code snippet for send_nvt.
 *
 * @param  x  Prefix for names in snippet.
 */
#define DEF(x)                                                    \
      const char* x = nvt_iterator_ ## x (nvts);                  \
      gchar* x ## _text = x                                       \
                          ? g_markup_escape_text (x, -1)          \
                          : g_strdup ("");

/**
 * @brief Send XML for an NVT.
 *
 * The caller must send the closing NVT tag.
 *
 * @param[in]  nvts        The NVT.
 * @param[in]  details     If true, detailed XML, else simple XML.
 * @param[in]  pref_count  Preference count.  Used if details is true.
 * @param[in]  timeout     Timeout.  Used if details is true.
 * @param[in]  write_to_client       Function to write to client.
 * @param[in]  write_to_client_data  Argument to \p write_to_client.
 *
 * @return TRUE if out of space in to_client buffer, else FALSE.
 */
static gboolean
send_nvt (iterator_t *nvts, int details, int pref_count, const char *timeout,
          int (*write_to_client) (void*), void* write_to_client_data)
{
  const char* oid = nvt_iterator_oid (nvts);
  const char* name = nvt_iterator_name (nvts);
  gchar* msg;

  gchar* name_text = g_markup_escape_text (name, strlen (name));
  if (details)
    {

#ifndef S_SPLINT_S
      DEF (copyright);
      DEF (description);
      DEF (summary);
      DEF (family);
      DEF (version);
      DEF (tag);
#endif /* not S_SPLINT_S */

#undef DEF

      msg = g_strdup_printf ("<nvt"
                             " oid=\"%s\">"
                             "<name>%s</name>"
                             "<category>%s</category>"
                             "<copyright>%s</copyright>"
                             "<description>%s</description>"
                             "<summary>%s</summary>"
                             "<family>%s</family>"
                             "<version>%s</version>"
                             "<cvss_base>%s</cvss_base>"
                             "<risk_factor>%s</risk_factor>"
                             "<cve_id>%s</cve_id>"
                             "<bugtraq_id>%s</bugtraq_id>"
                             "<xrefs>%s</xrefs>"
                             "<fingerprints>%s</fingerprints>"
                             "<tags>%s</tags>"
                             "<preference_count>%i</preference_count>"
                             "<timeout>%s</timeout>"
                             "<checksum>"
                             "<algorithm>md5</algorithm>"
                             /** @todo Implement checksum. */
                             "2397586ea5cd3a69f953836f7be9ef7b"
                             "</checksum>",
                             oid,
                             name_text,
                             category_name (nvt_iterator_category (nvts)),
                             copyright_text,
                             description_text,
                             summary_text,
                             family_text,
                             version_text,
                             nvt_iterator_cvss_base (nvts)
                              ? nvt_iterator_cvss_base (nvts)
                              : "",
                             nvt_iterator_risk_factor (nvts)
                              ? nvt_iterator_risk_factor (nvts)
                              : "",
                             nvt_iterator_cve (nvts),
                             nvt_iterator_bid (nvts),
                             nvt_iterator_xref (nvts),
                             nvt_iterator_sign_key_ids (nvts),
                             tag_text,
                             pref_count,
                             timeout ? timeout : "");
      g_free (copyright_text);
      g_free (description_text);
      g_free (summary_text);
      g_free (family_text);
      g_free (version_text);
      g_free (tag_text);
    }
  else
    msg = g_strdup_printf ("<nvt"
                           " oid=\"%s\">"
                           "<name>%s</name>"
                           "<checksum>"
                           "<algorithm>md5</algorithm>"
                            /** @todo Implement checksum. */
                           "2397586ea5cd3a69f953836f7be9ef7b"
                           "</checksum>",
                           oid,
                           name_text);
  g_free (name_text);
  if (send_to_client (msg, write_to_client, write_to_client_data))
    {
      g_free (msg);
      return TRUE;
    }
  g_free (msg);
  return FALSE;
}

/**
 * @brief Send XML for the reports of a task.
 *
 * @param[in]  task             The task.
 * @param[in]  apply_overrides  Whether to apply overrides.
 * @param[in]  write_to_client       Function to write to client.
 * @param[in]  write_to_client_data  Argument to \p write_to_client.
 *
 * @return 0 success, -4 out of space in to_client,
 *         -5 failed to get report counts, -6 failed to get timestamp.
 */
static int
send_reports (task_t task, int apply_overrides, int (*write_to_client) (void*),
              void* write_to_client_data)
{
  iterator_t iterator;
  report_t index;

  if (send_to_client ("<reports>", write_to_client, write_to_client_data))
    return -4;

  init_report_iterator (&iterator, task, 0);
  while (next_report (&iterator, &index))
    {
      gchar *uuid, *timestamp, *msg;
      int debugs, false_positives, holes, infos, logs, warnings, run_status;

      uuid = report_uuid (index);

      if (report_counts (uuid, &debugs, &holes, &infos, &logs, &warnings,
                         &false_positives, apply_overrides))
        {
          free (uuid);
          return -5;
        }

      if (report_timestamp (uuid, &timestamp))
        {
          free (uuid);
          return -6;
        }

      tracef ("     %s\n", uuid);

      report_scan_run_status (index, &run_status);
      msg = g_strdup_printf ("<report"
                             " id=\"%s\">"
                             "<timestamp>%s</timestamp>"
                             "<scan_run_status>%s</scan_run_status>"
                             "<result_count>"
                             "<debug>%i</debug>"
                             "<hole>%i</hole>"
                             "<info>%i</info>"
                             "<log>%i</log>"
                             "<warning>%i</warning>"
                             "<false_positive>%i</false_positive>"
                             "</result_count>"
                             "</report>",
                             uuid,
                             timestamp,
                             run_status_name
                              (run_status ? run_status
                                          : TASK_STATUS_INTERNAL_ERROR),
                             debugs,
                             holes,
                             infos,
                             logs,
                             warnings,
                             false_positives);
      g_free (timestamp);
      if (send_to_client (msg, write_to_client, write_to_client_data))
        {
          g_free (msg);
          free (uuid);
          return -4;
        }
      g_free (msg);
      free (uuid);
    }
  cleanup_iterator (&iterator);

  if (send_to_client ("</reports>", write_to_client, write_to_client_data))
    return -4;

  return 0;
}

/**
 * @brief Convert \n's to real newline's.
 *
 * @param[in]  text  The text in which to insert newlines.
 *
 * @return A newly allocated version of text.
 */
static gchar*
convert_to_newlines (const char *text)
{
  /** @todo Do this better. */

  gsize left = strlen (text);
  gchar *new, *ch;

  /* Allocate buffer of a safe length. */
  {
    new = g_strdup (text);
  }

  ch = new;
  while (*ch)
    {
      if (*ch == '\\')
        {
          ch++;
          switch (*ch)
            {
              case 'r':
                {
                  /* \r is flushed */
                  memmove (ch - 1, ch + 1, left);
                  left--;
                  ch -= 2;
                  break;
                }
              case 'n':
                {
                  /* \n becomes "\n" (one newline) */
                  memmove (ch, ch + 1, left);
                  left--;
                  *(ch - 1) = '\n';
                  ch--;
                  break;
                }
              default:
                {
                  ch--;
                  break;
                }
            }
        }
      ch++; left--;
    }
  return new;
}

/**
 * @brief Format XML into a buffer.
 *
 * @param[in]  buffer  Buffer.
 * @param[in]  format  Format string for XML.
 * @param[in]  ...     Arguments for format string.
 */
static void
buffer_xml_append_printf (GString *buffer, const char *format, ...)
{
  va_list args;
  gchar *msg;
  va_start (args, format);
  msg = g_markup_vprintf_escaped (format, args);
  va_end (args);
  g_string_append (buffer, msg);
  g_free (msg);
}

/**
 * @brief Buffer XML for some notes.
 *
 * @param[in]  buffer                 Buffer into which to buffer notes.
 * @param[in]  notes                  Notes iterator.
 * @param[in]  include_notes_details  Whether to include details of notes.
 * @param[in]  include_result         Whether to include associated result.
 */
static void
buffer_notes_xml (GString *buffer, iterator_t *notes, int include_notes_details,
                  int include_result)
{
  while (next (notes))
    {
      char *uuid_task, *uuid_result;

      if (note_iterator_task (notes))
        task_uuid (note_iterator_task (notes),
                   &uuid_task);
      else
        uuid_task = NULL;

      if (note_iterator_result (notes))
        result_uuid (note_iterator_result (notes),
                     &uuid_result);
      else
        uuid_result = NULL;

      if (include_notes_details == 0)
        {
          const char *text = note_iterator_text (notes);
          gchar *excerpt = g_strndup (text, 40);
          buffer_xml_append_printf (buffer,
                                    "<note id=\"%s\">"
                                    "<nvt oid=\"%s\">"
                                    "<name>%s</name>"
                                    "</nvt>"
                                    "<text excerpt=\"%i\">%s</text>"
                                    "<orphan>%i</orphan>"
                                    "</note>",
                                    note_iterator_uuid (notes),
                                    note_iterator_nvt_oid (notes),
                                    note_iterator_nvt_name (notes),
                                    strlen (excerpt) < strlen (text),
                                    excerpt,
                                    ((note_iterator_task (notes)
                                      && (uuid_task == NULL))
                                     || (note_iterator_result (notes)
                                         && (uuid_result == NULL))));
          g_free (excerpt);
        }
      else
        {
          char *name_task;
          time_t creation_time, mod_time;

          if (uuid_task)
            name_task = task_name (note_iterator_task (notes));
          else
            name_task = NULL;

          creation_time = note_iterator_creation_time (notes);
          mod_time = note_iterator_modification_time (notes);

          buffer_xml_append_printf
           (buffer,
            "<note id=\"%s\">"
            "<nvt oid=\"%s\"><name>%s</name></nvt>"
            "<creation_time>%s</creation_time>"
            "<modification_time>%s</modification_time>"
            "<text>%s</text>"
            "<hosts>%s</hosts>"
            "<port>%s</port>"
            "<threat>%s</threat>"
            "<task id=\"%s\"><name>%s</name></task>"
            "<orphan>%i</orphan>",
            note_iterator_uuid (notes),
            note_iterator_nvt_oid (notes),
            note_iterator_nvt_name (notes),
            ctime_strip_newline (&creation_time),
            ctime_strip_newline (&mod_time),
            note_iterator_text (notes),
            note_iterator_hosts (notes)
             ? note_iterator_hosts (notes) : "",
            note_iterator_port (notes)
             ? note_iterator_port (notes) : "",
            note_iterator_threat (notes)
             ? note_iterator_threat (notes) : "",
            uuid_task ? uuid_task : "",
            name_task ? name_task : "",
            ((note_iterator_task (notes) && (uuid_task == NULL))
             || (note_iterator_result (notes) && (uuid_result == NULL))));

          free (name_task);

          if (include_result && note_iterator_result (notes))
            {
              iterator_t results;

              init_result_iterator (&results, 0,
                                    note_iterator_result (notes),
                                    NULL, 0, 1, 1, NULL, NULL, NULL, NULL, 0);
              while (next (&results))
                buffer_results_xml (buffer,
                                    &results,
                                    0,
                                    0,  /* Notes. */
                                    0,  /* Note details. */
                                    0,  /* Overrides. */
                                    0); /* Override details. */
              cleanup_iterator (&results);

              buffer_xml_append_printf (buffer, "</note>");
            }
          else
            buffer_xml_append_printf (buffer,
                                      "<result id=\"%s\"/>"
                                      "</note>",
                                      uuid_result ? uuid_result : "");
        }
      free (uuid_task);
      free (uuid_result);
    }
}

/**
 * @brief Buffer XML for some overrides.
 *
 * @param[in]  buffer                     Buffer into which to buffer overrides.
 * @param[in]  overrides                  Overrides iterator.
 * @param[in]  include_overrides_details  Whether to include details of overrides.
 * @param[in]  include_result             Whether to include associated result.
 */
static void
buffer_overrides_xml (GString *buffer, iterator_t *overrides,
                      int include_overrides_details, int include_result)
{
  while (next (overrides))
    {
      char *uuid_task, *uuid_result;

      if (override_iterator_task (overrides))
        task_uuid (override_iterator_task (overrides),
                   &uuid_task);
      else
        uuid_task = NULL;

      if (override_iterator_result (overrides))
        result_uuid (override_iterator_result (overrides),
                     &uuid_result);
      else
        uuid_result = NULL;

      if (include_overrides_details == 0)
        {
          const char *text = override_iterator_text (overrides);
          gchar *excerpt = g_strndup (text, 40);
          buffer_xml_append_printf (buffer,
                                    "<override id=\"%s\">"
                                    "<nvt oid=\"%s\">"
                                    "<name>%s</name>"
                                    "</nvt>"
                                    "<text excerpt=\"%i\">%s</text>"
                                    "<threat>%s</threat>"
                                    "<new_threat>%s</new_threat>"
                                    "<orphan>%i</orphan>"
                                    "</override>",
                                    override_iterator_uuid (overrides),
                                    override_iterator_nvt_oid (overrides),
                                    override_iterator_nvt_name (overrides),
                                    strlen (excerpt) < strlen (text),
                                    excerpt,
                                    override_iterator_threat (overrides)
                                     ? override_iterator_threat (overrides)
                                     : "",
                                    override_iterator_new_threat (overrides),
                                    ((override_iterator_task (overrides)
                                      && (uuid_task == NULL))
                                     || (override_iterator_result (overrides)
                                         && (uuid_result == NULL))));
          g_free (excerpt);
        }
      else
        {
          char *name_task;
          time_t creation_time, mod_time;

          if (uuid_task)
            name_task = task_name (override_iterator_task (overrides));
          else
            name_task = NULL;

          creation_time = override_iterator_creation_time (overrides);
          mod_time = override_iterator_modification_time (overrides);

          buffer_xml_append_printf
           (buffer,
            "<override id=\"%s\">"
            "<nvt oid=\"%s\"><name>%s</name></nvt>"
            "<creation_time>%s</creation_time>"
            "<modification_time>%s</modification_time>"
            "<text>%s</text>"
            "<hosts>%s</hosts>"
            "<port>%s</port>"
            "<threat>%s</threat>"
            "<new_threat>%s</new_threat>"
            "<task id=\"%s\"><name>%s</name></task>"
            "<orphan>%i</orphan>",
            override_iterator_uuid (overrides),
            override_iterator_nvt_oid (overrides),
            override_iterator_nvt_name (overrides),
            ctime_strip_newline (&creation_time),
            ctime_strip_newline (&mod_time),
            override_iterator_text (overrides),
            override_iterator_hosts (overrides)
             ? override_iterator_hosts (overrides) : "",
            override_iterator_port (overrides)
             ? override_iterator_port (overrides) : "",
            override_iterator_threat (overrides)
             ? override_iterator_threat (overrides) : "",
            override_iterator_new_threat (overrides),
            uuid_task ? uuid_task : "",
            name_task ? name_task : "",
            ((override_iterator_task (overrides) && (uuid_task == NULL))
             || (override_iterator_result (overrides) && (uuid_result == NULL))));

          free (name_task);

          if (include_result && override_iterator_result (overrides))
            {
              iterator_t results;

              init_result_iterator (&results, 0,
                                    override_iterator_result (overrides),
                                    NULL, 0, 1, 1, NULL, NULL, NULL, NULL, 0);
              while (next (&results))
                buffer_results_xml (buffer,
                                    &results,
                                    0,
                                    0,  /* Notes. */
                                    0,  /* Note details. */
                                    0,  /* Overrides. */
                                    0); /* Override details. */
              cleanup_iterator (&results);

              buffer_xml_append_printf (buffer, "</override>");
            }
          else
            buffer_xml_append_printf (buffer,
                                      "<result id=\"%s\"/>"
                                      "</override>",
                                      uuid_result ? uuid_result : "");
        }
      free (uuid_task);
      free (uuid_result);
    }
}

/* External for manage.c. */
/**
 * @brief Buffer XML for the NVT preference of a config.
 *
 * @param[in]  buffer  Buffer.
 * @param[in]  prefs   NVT preference iterator.
 * @param[in]  config  Config.
 */
void
buffer_config_preference_xml (GString *buffer, iterator_t *prefs,
                              config_t config)
{
  char *real_name, *type, *value, *nvt;
  char *oid = NULL;

  real_name = nvt_preference_iterator_real_name (prefs);
  type = nvt_preference_iterator_type (prefs);
  value = nvt_preference_iterator_config_value (prefs, config);
  nvt = nvt_preference_iterator_nvt (prefs);

  if (nvt) oid = nvt_oid (nvt);

  buffer_xml_append_printf (buffer,
                            "<preference>"
                            "<nvt oid=\"%s\"><name>%s</name></nvt>"
                            "<name>%s</name>"
                            "<type>%s</type>",
                            oid ? oid : "",
                            nvt ? nvt : "",
                            real_name ? real_name : "",
                            type ? type : "");

  if (value
      && type
      && (strcmp (type, "radio") == 0))
    {
      /* Handle the other possible values. */
      char *pos = strchr (value, ';');
      if (pos) *pos = '\0';
      buffer_xml_append_printf (buffer, "<value>%s</value>", value);
      while (pos)
        {
          char *pos2 = strchr (++pos, ';');
          if (pos2) *pos2 = '\0';
          buffer_xml_append_printf (buffer, "<alt>%s</alt>", pos);
          pos = pos2;
        }
    }
  else if (value
           && type
           && (strcmp (type, "password") == 0))
    buffer_xml_append_printf (buffer, "<value></value>");
  else
    buffer_xml_append_printf (buffer, "<value>%s</value>", value ? value : "");

  buffer_xml_append_printf (buffer, "</preference>");

  free (real_name);
  free (type);
  free (value);
  free (nvt);
  free (oid);
}

/** @todo Exported for manage_sql.c. */
/**
 * @brief Buffer XML for some results.
 *
 * @param[in]  buffer                 Buffer into which to buffer results.
 * @param[in]  results                Result iterator.
 * @param[in]  task                   Task associated with results.  Only
 *                                    needed with include_notes or
 *                                    include_overrides.
 * @param[in]  include_notes          Whether to include notes.
 * @param[in]  include_notes_details  Whether to include details of notes.
 * @param[in]  include_overrides          Whether to include overrides.
 * @param[in]  include_overrides_details  Whether to include details of overrides.
 */
void
buffer_results_xml (GString *buffer, iterator_t *results, task_t task,
                    int include_notes, int include_notes_details,
                    int include_overrides, int include_overrides_details)
{
  const char *descr = result_iterator_descr (results);
  gchar *nl_descr = descr ? convert_to_newlines (descr) : NULL;
  const char *name = result_iterator_nvt_name (results);
  const char *cvss_base = result_iterator_nvt_cvss_base (results);
  const char *risk_factor = result_iterator_nvt_risk_factor (results);
  const char *cve = result_iterator_nvt_cve (results);
  const char *bid = result_iterator_nvt_bid (results);
  char *uuid;

  result_uuid (result_iterator_result (results), &uuid);

  buffer_xml_append_printf
   (buffer,
    "<result id=\"%s\">"
    "<subnet>%s</subnet>"
    "<host>%s</host>"
    "<port>%s</port>"
    "<nvt oid=\"%s\">"
    "<name>%s</name>"
    "<cvss_base>%s</cvss_base>"
    "<risk_factor>%s</risk_factor>"
    "<cve>%s</cve>"
    "<bid>%s</bid>"
    "</nvt>"
    "<threat>%s</threat>"
    "<description>%s</description>",
    uuid,
    result_iterator_subnet (results),
    result_iterator_host (results),
    result_iterator_port (results),
    result_iterator_nvt_oid (results),
    name ? name : "",
    cvss_base ? cvss_base : "",
    risk_factor ? risk_factor : "",
    cve ? cve : "",
    bid ? bid : "",
    manage_result_type_threat (result_iterator_type (results)),
    descr ? nl_descr : "");

  if (include_overrides)
    buffer_xml_append_printf (buffer,
                              "<original_threat>%s</original_threat>",
                              manage_result_type_threat
                               (result_iterator_original_type (results)));

  free (uuid);

  if (descr) g_free (nl_descr);

  if (include_notes)
    {
      iterator_t notes;

      assert (task);

      g_string_append (buffer, "<notes>");

      init_note_iterator (&notes,
                          0,
                          0,
                          result_iterator_result (results),
                          task,
                          0, /* Most recent first. */
                          "creation_time");
      buffer_notes_xml (buffer, &notes, include_notes_details, 0);
      cleanup_iterator (&notes);

      g_string_append (buffer, "</notes>");
    }

  if (include_overrides)
    {
      iterator_t overrides;

      assert (task);

      g_string_append (buffer, "<overrides>");

      init_override_iterator (&overrides,
                              0,
                              0,
                              result_iterator_result (results),
                              task,
                              0, /* Most recent first. */
                              "creation_time");
      buffer_overrides_xml (buffer,
                            &overrides,
                            include_overrides_details,
                            0);
      cleanup_iterator (&overrides);

      g_string_append (buffer, "</overrides>");
    }

  g_string_append (buffer, "</result>");
}

/**
 * @brief Buffer XML for some schedules.
 *
 * @param[in]  buffer           Buffer.
 * @param[in]  schedules        Schedules iterator.
 * @param[in]  include_details  Whether to include details.
 */
static void
buffer_schedules_xml (GString *buffer, iterator_t *schedules,
                      int include_details)
{
  while (next (schedules))
    {
      if (include_details == 0)
        {
          buffer_xml_append_printf (buffer,
                                    "<schedule id=\"%s\">"
                                    "<name>%s</name>"
                                    "</schedule>",
                                    schedule_iterator_uuid (schedules),
                                    schedule_iterator_name (schedules));
        }
      else
        {
          iterator_t tasks;
          time_t first_time = schedule_iterator_first_time (schedules);
          time_t next_time = schedule_iterator_next_time (schedules);
          gchar *first_ctime = g_strdup (ctime_strip_newline (&first_time));

          buffer_xml_append_printf
           (buffer,
            "<schedule id=\"%s\">"
            "<name>%s</name>"
            "<comment>%s</comment>"
            "<first_time>%s</first_time>"
            "<next_time>%s</next_time>"
            "<period>%i</period>"
            "<period_months>%i</period_months>"
            "<duration>%i</duration>"
            "<in_use>%i</in_use>",
            schedule_iterator_uuid (schedules),
            schedule_iterator_name (schedules),
            schedule_iterator_comment (schedules),
            first_ctime,
            (next_time == 0 ? "over" : ctime_strip_newline (&next_time)),
            schedule_iterator_period (schedules),
            schedule_iterator_period_months (schedules),
            schedule_iterator_duration (schedules),
            schedule_iterator_in_use (schedules));

          g_free (first_ctime);

          buffer_xml_append_printf (buffer, "<tasks>");
          init_schedule_task_iterator (&tasks,
                                       schedule_iterator_schedule (schedules));
          while (next (&tasks))
            buffer_xml_append_printf (buffer,
                                      "<task id=\"%s\">"
                                      "<name>%s</name>"
                                      "</task>",
                                      schedule_task_iterator_uuid (&tasks),
                                      schedule_task_iterator_name (&tasks));
          cleanup_iterator (&tasks);
          buffer_xml_append_printf (buffer,
                                    "</tasks>"
                                    "</schedule>");
        }
    }
}

/**
 * @brief Handle the end of an OMP XML element.
 *
 * React to the end of an XML element according to the current value
 * of \ref client_state, usually adjusting \ref client_state to indicate
 * the change (with \ref set_client_state).  Call \ref send_to_client to queue
 * any responses for the client.  Call the task utilities to adjust the
 * tasks (for example \ref start_task, \ref stop_task, \ref set_task_parameter,
 * \ref delete_task and \ref find_task ).
 *
 * Set error parameter on encountering an error.
 *
 * @param[in]  context           Parser context.
 * @param[in]  element_name      XML element name.
 * @param[in]  user_data         OMP parser.
 * @param[in]  error             Error parameter.
 */
static void
omp_xml_handle_end_element (/*@unused@*/ GMarkupParseContext* context,
                            const gchar *element_name,
                            gpointer user_data,
                            GError **error)
{
  omp_parser_t *omp_parser = (omp_parser_t*) user_data;
  int (*write_to_client) (void*) = (int (*) (void*)) omp_parser->client_writer;
  void* write_to_client_data = (void*) omp_parser->client_writer_data;

  tracef ("   XML    end: %s\n", element_name);

  switch (client_state)
    {
      case CLIENT_TOP:
        assert (0);
        break;

      case CLIENT_AUTHENTICATE:
        switch (authenticate (&current_credentials))
          {
            case 0:   /* Authentication succeeded. */
              if (load_tasks ())
                {
                  g_warning ("%s: failed to load tasks\n", __FUNCTION__);
                  g_set_error (error, G_MARKUP_ERROR, G_MARKUP_ERROR_PARSE,
                               "Manager failed to load tasks.");
                  free_credentials (&current_credentials);
                  SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("authenticate"));
                  set_client_state (CLIENT_TOP);
                }
              else
                {
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("authenticate"));
                  set_client_state (CLIENT_AUTHENTIC);
                }
              break;
            case 1:   /* Authentication failed. */
              free_credentials (&current_credentials);
              SEND_TO_CLIENT_OR_FAIL (XML_ERROR_AUTH_FAILED ("authenticate"));
              set_client_state (CLIENT_TOP);
              break;
            case -1:  /* Error while authenticating. */
            default:
              free_credentials (&current_credentials);
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("authenticate"));
              set_client_state (CLIENT_TOP);
              break;
          }
        break;

      case CLIENT_AUTHENTICATE_CREDENTIALS:
        assert (strcasecmp ("CREDENTIALS", element_name) == 0);
        set_client_state (CLIENT_AUTHENTICATE);
        break;

      case CLIENT_AUTHENTICATE_CREDENTIALS_USERNAME:
        assert (strcasecmp ("USERNAME", element_name) == 0);
        set_client_state (CLIENT_AUTHENTICATE_CREDENTIALS);
        break;

      case CLIENT_AUTHENTICATE_CREDENTIALS_PASSWORD:
        assert (strcasecmp ("PASSWORD", element_name) == 0);
        set_client_state (CLIENT_AUTHENTICATE_CREDENTIALS);
        break;

      case CLIENT_AUTHENTIC:
      case CLIENT_COMMANDS:
      case CLIENT_AUTHENTIC_COMMANDS:
        assert (strcasecmp ("COMMANDS", element_name) == 0);
        SENDF_TO_CLIENT_OR_FAIL ("</commands_response>");
        break;

      case CLIENT_GET_PREFERENCES:
        {
          iterator_t prefs;
          nvt_t nvt = 0;
          config_t config = 0;
          if (get_preferences_data->nvt_oid
              && find_nvt (get_preferences_data->nvt_oid, &nvt))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_preferences"));
          else if (get_preferences_data->nvt_oid && nvt == 0)
            {
              if (send_find_error_to_client ("get_preferences",
                                             "NVT",
                                             get_preferences_data->nvt_oid,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (get_preferences_data->config_id
                   && find_config (get_preferences_data->config_id, &config))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_preferences"));
          else if (get_preferences_data->config_id && config == 0)
            {
              if (send_find_error_to_client ("get_preferences",
                                             "config",
                                             get_preferences_data->config_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              char *nvt_name = manage_nvt_name (nvt);
              SEND_TO_CLIENT_OR_FAIL ("<get_preferences_response"
                                      " status=\"" STATUS_OK "\""
                                      " status_text=\"" STATUS_OK_TEXT "\">");
              init_nvt_preference_iterator (&prefs, nvt_name);
              free (nvt_name);
              if (get_preferences_data->preference)
                while (next (&prefs))
                  {
                    char *name = strstr (nvt_preference_iterator_name (&prefs), "]:");
                    if (name
                        && (strcmp (name + 2,
                                    get_preferences_data->preference)
                            == 0))
                      {
                        if (config)
                          {
                            GString *buffer = g_string_new ("");
                            buffer_config_preference_xml (buffer, &prefs, config);
                            SEND_TO_CLIENT_OR_FAIL (buffer->str);
                            g_string_free (buffer, TRUE);
                          }
                        else
                          SENDF_TO_CLIENT_OR_FAIL ("<preference>"
                                                   "<name>%s</name>"
                                                   "<value>%s</value>"
                                                   "</preference>",
                                                   nvt_preference_iterator_name (&prefs),
                                                   nvt_preference_iterator_value (&prefs));
                        break;
                      }
                  }
              else
                while (next (&prefs))
                  if (config)
                    {
                      GString *buffer = g_string_new ("");
                      buffer_config_preference_xml (buffer, &prefs, config);
                      SEND_TO_CLIENT_OR_FAIL (buffer->str);
                      g_string_free (buffer, TRUE);
                    }
                  else
                    SENDF_TO_CLIENT_OR_FAIL ("<preference>"
                                             "<name>%s</name>"
                                             "<value>%s</value>"
                                             "</preference>",
                                             nvt_preference_iterator_name (&prefs),
                                             nvt_preference_iterator_value (&prefs));
              cleanup_iterator (&prefs);
              SEND_TO_CLIENT_OR_FAIL ("</get_preferences_response>");
            }
          get_preferences_data_reset (get_preferences_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

#if 0
      case CLIENT_GET_CERTIFICATES:
        if (scanner.certificates)
          {
            SEND_TO_CLIENT_OR_FAIL ("<get_certificates_response"
                                    " status=\"" STATUS_OK "\""
                                    " status_text=\"" STATUS_OK_TEXT "\">");
            if (certificates_find (scanner.certificates,
                                   send_certificate,
                                   NULL))
              {
                error_send_to_client (error);
                return;
              }
            SEND_TO_CLIENT_OR_FAIL ("</get_certificates_response>");
          }
        else
          SEND_TO_CLIENT_OR_FAIL (XML_SERVICE_DOWN ("get_certificates"));
        set_client_state (CLIENT_AUTHENTIC);
        break;
#endif

      case CLIENT_GET_DEPENDENCIES:
        if (scanner.plugins_dependencies)
          {
            nvt_t nvt = 0;

            if (get_dependencies_data->nvt_oid
                && find_nvt (get_dependencies_data->nvt_oid, &nvt))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_dependencies"));
            else if (get_dependencies_data->nvt_oid && nvt == 0)
              {
                if (send_find_error_to_client ("get_dependencies",
                                               "NVT",
                                               get_dependencies_data->nvt_oid,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else
              {
                void* data[2];

                data[0] = (int (*) (void*)) write_to_client;
                data[1] = (void*) write_to_client_data;

                SEND_TO_CLIENT_OR_FAIL ("<get_dependencies_response"
                                        " status=\"" STATUS_OK "\""
                                        " status_text=\"" STATUS_OK_TEXT "\">");
                if (nvt)
                  {
                    char *name = manage_nvt_name (nvt);

                    if (name)
                      {
                        gpointer value;
                        value = g_hash_table_lookup
                                 (scanner.plugins_dependencies,
                                  name);
                        if (value && send_dependency (name, value, data))
                          {
                            g_free (name);
                            error_send_to_client (error);
                            return;
                          }
                        g_free (name);
                      }
                  }
                else if (g_hash_table_find (scanner.plugins_dependencies,
                                            send_dependency,
                                            data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                SEND_TO_CLIENT_OR_FAIL ("</get_dependencies_response>");
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL (XML_SERVICE_DOWN ("get_dependencies"));
        get_dependencies_data_reset (get_dependencies_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_GET_NOTES:
        {
          note_t note = 0;
          nvt_t nvt = 0;
          task_t task = 0;

          assert (strcasecmp ("GET_NOTES", element_name) == 0);

          if (get_notes_data->note_id && get_notes_data->nvt_oid)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_notes",
                                "Only one of NVT and the note_id attribute"
                                " may be given"));
          else if (get_notes_data->note_id && get_notes_data->task_id)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_notes",
                                "Only one of the note_id and task_id"
                                " attributes may be given"));
          else if (get_notes_data->note_id
              && find_note (get_notes_data->note_id, &note))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_notes"));
          else if (get_notes_data->note_id && note == 0)
            {
              if (send_find_error_to_client ("get_notes",
                                             "note",
                                             get_notes_data->note_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (get_notes_data->task_id
                   && find_task (get_notes_data->task_id, &task))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_notes"));
          else if (get_notes_data->task_id && task == 0)
            {
              if (send_find_error_to_client ("get_notes",
                                             "task",
                                             get_notes_data->task_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (get_notes_data->nvt_oid
                   && find_nvt (get_notes_data->nvt_oid, &nvt))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_notes"));
          else if (get_notes_data->nvt_oid && nvt == 0)
            {
              if (send_find_error_to_client ("get_notes",
                                             "NVT",
                                             get_notes_data->nvt_oid,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              iterator_t notes;
              GString *buffer;

              SENDF_TO_CLIENT_OR_FAIL ("<get_notes_response"
                                       " status=\"" STATUS_OK "\""
                                       " status_text=\"" STATUS_OK_TEXT "\">");

              buffer = g_string_new ("");

              init_note_iterator (&notes,
                                  note,
                                  nvt,
                                  0,
                                  task,
                                  get_notes_data->sort_order,
                                  get_notes_data->sort_field);
              buffer_notes_xml (buffer, &notes, get_notes_data->details,
                                get_notes_data->result);
              cleanup_iterator (&notes);

              SEND_TO_CLIENT_OR_FAIL (buffer->str);
              g_string_free (buffer, TRUE);

              SEND_TO_CLIENT_OR_FAIL ("</get_notes_response>");
            }

          get_notes_data_reset (get_notes_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_NVT_FEED_CHECKSUM:
        {
          char *md5sum;
          if (get_nvt_feed_checksum_data->algorithm
              && strcasecmp (get_nvt_feed_checksum_data->algorithm, "md5"))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_nvt_feed_checksum",
                                "GET_NVT_FEED_CHECKSUM algorithm must be md5"));
          else if ((md5sum = nvts_md5sum ()))
            {
              SEND_TO_CLIENT_OR_FAIL ("<get_nvt_feed_checksum_response"
                                      " status=\"" STATUS_OK "\""
                                      " status_text=\"" STATUS_OK_TEXT "\">"
                                      "<checksum algorithm=\"md5\">");
              SEND_TO_CLIENT_OR_FAIL (md5sum);
              free (md5sum);
              SEND_TO_CLIENT_OR_FAIL ("</checksum>"
                                      "</get_nvt_feed_checksum_response>");
            }
          else
            SEND_TO_CLIENT_OR_FAIL (XML_SERVICE_DOWN ("get_nvt_feed_checksum"));
          get_nvt_feed_checksum_data_reset (get_nvt_feed_checksum_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_NVTS:
        {
          char *md5sum = nvts_md5sum ();
          if (md5sum)
            {
              config_t config = (config_t) 0;
              nvt_t nvt = 0;

              free (md5sum);

              if (get_nvts_data->nvt_oid && get_nvts_data->family)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("get_nvts",
                                    "Too many parameters at once"));
              else if ((get_nvts_data->details == 0)
                       && get_nvts_data->preference_count)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("get_nvts",
                                    "GET_NVTS preference_count attribute"
                                    " requires the details attribute"));
              else if (((get_nvts_data->details == 0)
                        || (get_nvts_data->config_id == NULL))
                       && get_nvts_data->preferences)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("get_nvts",
                                    "GET_NVTS preferences attribute"
                                    " requires the details and config_id"
                                    " attributes"));
              else if (((get_nvts_data->details == 0)
                        || (get_nvts_data->config_id == NULL))
                       && get_nvts_data->timeout)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("get_nvts",
                                    "GET_NVTS timeout attribute"
                                    " requires the details and config_id"
                                    " attributes"));
              else if (get_nvts_data->nvt_oid
                       && find_nvt (get_nvts_data->nvt_oid, &nvt))
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("get_nvts"));
              else if (get_nvts_data->nvt_oid && nvt == 0)
                {
                  if (send_find_error_to_client ("get_nvts",
                                                 "NVT",
                                                 get_nvts_data->nvt_oid,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                }
              else if (get_nvts_data->config_id
                       && find_config (get_nvts_data->config_id,
                                       &config))
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("get_nvts"));
              else if (get_nvts_data->config_id && (config == 0))
                {
                  if (send_find_error_to_client
                       ("get_nvts",
                        "config",
                        get_nvts_data->config_id,
                        write_to_client,
                        write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                }
              else
                {
                  iterator_t nvts;

                  SENDF_TO_CLIENT_OR_FAIL
                   ("<get_nvts_response"
                    " status=\"" STATUS_OK "\""
                    " status_text=\"" STATUS_OK_TEXT "\">");

                  init_nvt_iterator (&nvts,
                                     nvt,
                                     get_nvts_data->nvt_oid
                                      /* Presume the NVT is in the config (if
                                       * a config was given). */
                                      ? 0
                                      : config,
                                     get_nvts_data->family,
                                     get_nvts_data->sort_order,
                                     get_nvts_data->sort_field);
                  if (get_nvts_data->details)
                    while (next (&nvts))
                      {
                        int pref_count = -1;
                        char *timeout = NULL;

                        if (get_nvts_data->timeout)
                          timeout = config_nvt_timeout (config,
                                                        nvt_iterator_oid (&nvts));

                        if (get_nvts_data->preference_count)
                          {
                            const char *nvt_name = nvt_iterator_name (&nvts);
                            pref_count = nvt_preference_count (nvt_name);
                          }
                        if (send_nvt (&nvts, 1, pref_count, timeout,
                                      write_to_client, write_to_client_data))
                          {
                            cleanup_iterator (&nvts);
                            error_send_to_client (error);
                            return;
                          }

                        if (get_nvts_data->preferences)
                          {
                            iterator_t prefs;
                            const char *nvt_name = nvt_iterator_name (&nvts);

                            if (timeout == NULL)
                              timeout = config_nvt_timeout
                                         (config,
                                          nvt_iterator_oid (&nvts));

                            /* Send the preferences for the NVT. */

                            SENDF_TO_CLIENT_OR_FAIL ("<preferences>"
                                                     "<timeout>%s</timeout>",
                                                     timeout ? timeout : "");
                            free (timeout);

                            init_nvt_preference_iterator (&prefs, nvt_name);
                            while (next (&prefs))
                              {
                                GString *buffer = g_string_new ("");
                                buffer_config_preference_xml (buffer, &prefs, config);
                                SEND_TO_CLIENT_OR_FAIL (buffer->str);
                                g_string_free (buffer, TRUE);
                              }
                            cleanup_iterator (&prefs);

                            SEND_TO_CLIENT_OR_FAIL ("</preferences>");
                          }

                        SEND_TO_CLIENT_OR_FAIL ("</nvt>");
                      }
                  else
                    while (next (&nvts))
                      {
                        if (send_nvt (&nvts, 0, -1, NULL, write_to_client,
                                      write_to_client_data))
                          {
                            cleanup_iterator (&nvts);
                            error_send_to_client (error);
                            return;
                          }
                        SEND_TO_CLIENT_OR_FAIL ("</nvt>");
                      }
                  cleanup_iterator (&nvts);

                  SEND_TO_CLIENT_OR_FAIL ("</get_nvts_response>");
                }
            }
          else
            SEND_TO_CLIENT_OR_FAIL (XML_SERVICE_DOWN ("get_nvts"));
        }
        get_nvts_data_reset (get_nvts_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_GET_NVT_FAMILIES:
        {
          iterator_t families;

          SEND_TO_CLIENT_OR_FAIL ("<get_nvt_families_response"
                                  " status=\"" STATUS_OK "\""
                                  " status_text=\"" STATUS_OK_TEXT "\">"
                                  "<families>");

          init_family_iterator (&families,
                                1,
                                NULL,
                                get_nvt_families_data->sort_order);
          while (next (&families))
            {
              int family_max;
              const char *family;

              family = family_iterator_name (&families);
              if (family)
                family_max = family_nvt_count (family);
              else
                family_max = -1;

              SENDF_TO_CLIENT_OR_FAIL
               ("<family>"
                "<name>%s</name>"
                /* The total number of NVT's in the family. */
                "<max_nvt_count>%i</max_nvt_count>"
                "</family>",
                family ? family : "",
                family_max);
            }
          cleanup_iterator (&families);

          SEND_TO_CLIENT_OR_FAIL ("</families>"
                                  "</get_nvt_families_response>");
        }
        get_nvt_families_data_reset (get_nvt_families_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_GET_OVERRIDES:
        {
          override_t override = 0;
          nvt_t nvt = 0;
          task_t task = 0;

          assert (strcasecmp ("GET_OVERRIDES", element_name) == 0);

          if (get_overrides_data->override_id && get_overrides_data->nvt_oid)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_overrides",
                                "Only one of NVT and the override_id attribute"
                                " may be given"));
          else if (get_overrides_data->override_id && get_overrides_data->task_id)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_overrides",
                                "Only one of the override_id and task_id"
                                " attributes may be given"));
          else if (get_overrides_data->override_id
              && find_override (get_overrides_data->override_id, &override))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_overrides"));
          else if (get_overrides_data->override_id && override == 0)
            {
              if (send_find_error_to_client ("get_overrides",
                                             "override",
                                             get_overrides_data->override_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (get_overrides_data->task_id
                   && find_task (get_overrides_data->task_id, &task))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_overrides"));
          else if (get_overrides_data->task_id && task == 0)
            {
              if (send_find_error_to_client ("get_overrides",
                                             "task",
                                             get_overrides_data->task_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (get_overrides_data->nvt_oid
                   && find_nvt (get_overrides_data->nvt_oid, &nvt))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_overrides"));
          else if (get_overrides_data->nvt_oid && nvt == 0)
            {
              if (send_find_error_to_client ("get_overrides",
                                             "NVT",
                                             get_overrides_data->nvt_oid,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              iterator_t overrides;
              GString *buffer;

              SENDF_TO_CLIENT_OR_FAIL ("<get_overrides_response"
                                       " status=\"" STATUS_OK "\""
                                       " status_text=\"" STATUS_OK_TEXT "\">");

              buffer = g_string_new ("");

              init_override_iterator (&overrides,
                                      override,
                                      nvt,
                                      0,
                                      task,
                                      get_overrides_data->sort_order,
                                      get_overrides_data->sort_field);
              buffer_overrides_xml (buffer, &overrides, get_overrides_data->details,
                                    get_overrides_data->result);
              cleanup_iterator (&overrides);

              SEND_TO_CLIENT_OR_FAIL (buffer->str);
              g_string_free (buffer, TRUE);

              SEND_TO_CLIENT_OR_FAIL ("</get_overrides_response>");
            }

          get_overrides_data_reset (get_overrides_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_DELETE_NOTE:
        assert (strcasecmp ("DELETE_NOTE", element_name) == 0);
        if (delete_note_data->note_id)
          {
            note_t note;

            if (find_note (delete_note_data->note_id, &note))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_note"));
            else if (note == 0)
              {
                if (send_find_error_to_client ("delete_note",
                                               "note",
                                               delete_note_data->note_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (delete_note (note))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_note"));
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("delete_note"));
                  break;
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_note",
                              "DELETE_NOTE requires a note_id attribute"));
        delete_note_data_reset (delete_note_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_OVERRIDE:
        assert (strcasecmp ("DELETE_OVERRIDE", element_name) == 0);
        if (delete_override_data->override_id)
          {
            override_t override;

            if (find_override (delete_override_data->override_id, &override))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_override"));
            else if (override == 0)
              {
                if (send_find_error_to_client
                     ("delete_override",
                      "override",
                      delete_override_data->override_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (delete_override (override))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_override"));
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("delete_override"));
                  break;
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_override",
                              "DELETE_OVERRIDE requires a override_id attribute"));
        delete_override_data_reset (delete_override_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_REPORT:
        assert (strcasecmp ("DELETE_REPORT", element_name) == 0);
        if (delete_report_data->report_id)
          {
            report_t report;

            /** @todo Check syntax of delete_report_data->report_id and reply with
             *        STATUS_ERROR_SYNTAX.
             *
             *        This is a common situation.  If it changes here then all
             *        the commands must change.
             */
            if (find_report (delete_report_data->report_id, &report))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_report"));
            else if (report == 0)
              {
                if (send_find_error_to_client ("delete_report",
                                               "report",
                                               delete_report_data->report_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (manage_delete_report (report))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_report"));
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("delete_report",
                                      "Attempt to delete a hidden report"));
                  break;
                case 2:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("delete_report",
                                      "Report is in use"));
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("delete_report"));
                  break;
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_report",
                              "DELETE_REPORT requires a report_id attribute"));
        delete_report_data_reset (delete_report_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_REPORT_FORMAT:
        assert (strcasecmp ("DELETE_REPORT_FORMAT", element_name) == 0);
        if (delete_report_format_data->report_format_id)
          {
            report_format_t report_format;

            if (find_report_format (delete_report_format_data->report_format_id,
                                    &report_format))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_report_format"));
            else if (report_format == 0)
              {
                if (send_find_error_to_client
                     ("delete_report_format",
                      "report format",
                      delete_report_format_data->report_format_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (delete_report_format (report_format))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_report_format"));
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("delete_report_format",
                                      "Attempt to delete a hidden report"
                                      " format"));
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("delete_report_format"));
                  break;
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_report_format",
                              "DELETE_REPORT_FORMAT requires a report_format_id"
                              " attribute"));
        delete_report_format_data_reset (delete_report_format_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_SCHEDULE:
        assert (strcasecmp ("DELETE_SCHEDULE", element_name) == 0);
        if (delete_schedule_data->schedule_id)
          {
            schedule_t schedule;

            if (find_schedule (delete_schedule_data->schedule_id, &schedule))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_schedule"));
            else if (schedule == 0)
              {
                if (send_find_error_to_client
                     ("delete_schedule",
                      "schedule",
                      delete_schedule_data->schedule_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (delete_schedule (schedule))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_schedule"));
                  g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                         "Schedule %s has been deleted",
                         delete_schedule_data->schedule_id);
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("delete_schedule",
                                      "Schedule is in use"));
                  g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                         "Schedule %s could not be deleted",
                         delete_schedule_data->schedule_id);
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("delete_schedule"));
                  g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                         "Schedule %s could not be deleted",
                         delete_schedule_data->schedule_id);
                  break;
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_schedule",
                              "DELETE_SCHEDULE requires a schedule_id"
                              " attribute"));
        delete_schedule_data_reset (delete_schedule_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_GET_REPORTS:
        assert (strcasecmp ("GET_REPORTS", element_name) == 0);
        if (current_credentials.username == NULL)
          {
            get_reports_data_reset (get_reports_data);
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_reports"));
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        report_t request_report = 0, report;
        report_format_t report_format;
        iterator_t results;
        float min_cvss_base;
        iterator_t reports;

        if (get_reports_data->report_id
            && find_report (get_reports_data->report_id, &request_report))
          {
            get_reports_data_reset (get_reports_data);
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_reports"));
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        if (get_reports_data->format_id == NULL)
          get_reports_data->format_id
           = g_strdup ("d5da9f67-8551-4e51-807b-b6a873d70e34");

        if (find_report_format (get_reports_data->format_id, &report_format))
          {
            get_reports_data_reset (get_reports_data);
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_reports"));
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        if (report_format == 0)
          {
            if (send_find_error_to_client ("get_reports",
                                           "report format",
                                           get_reports_data->format_id,
                                           write_to_client,
                                           write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            get_reports_data_reset (get_reports_data);
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        if (get_reports_data->report_id && request_report == 0)
          {
            if (send_find_error_to_client ("get_reports",
                                           "report",
                                           get_reports_data->report_id,
                                           write_to_client,
                                           write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            get_reports_data_reset (get_reports_data);
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        if (get_reports_data->min_cvss_base
            && strlen (get_reports_data->min_cvss_base)
            && (sscanf (get_reports_data->min_cvss_base,
                        "%f",
                        &min_cvss_base)
                != 1))
          {
            get_reports_data_reset (get_reports_data);
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_reports",
                                "GET_REPORTS min_cvss_base must be a float"
                                " or the empty string"));
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        if (report_format_active (report_format) == 0)
          {
            get_reports_data_reset (get_reports_data);
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_reports",
                                "GET_REPORTS report format must be active"));
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        SEND_TO_CLIENT_OR_FAIL
         ("<get_reports_response"
          " status=\"" STATUS_OK "\""
          " status_text=\"" STATUS_OK_TEXT "\">");
        init_report_iterator (&reports, 0, request_report);
        while (next_report (&reports, &report))
          {
            gchar *extension, *content_type, *output;
            gsize output_len;

            output = manage_report (report,
                                    report_format,
                                    get_reports_data->sort_order,
                                    get_reports_data->sort_field,
                                    get_reports_data->result_hosts_only,
                                    get_reports_data->min_cvss_base,
                                    get_reports_data->levels,
                                    get_reports_data->apply_overrides,
                                    get_reports_data->search_phrase,
                                    get_reports_data->notes,
                                    get_reports_data->notes_details,
                                    get_reports_data->overrides,
                                    get_reports_data->overrides_details,
                                    get_reports_data->first_result,
                                    get_reports_data->max_results,
                                    &output_len,
                                    &extension,
                                    &content_type);
            if (output == NULL)
              {
                cleanup_iterator (&reports);
                internal_error_send_to_client (error);
                get_reports_data_reset (get_reports_data);
                set_client_state (CLIENT_AUTHENTIC);
                return;
              }

            SENDF_TO_CLIENT_OR_FAIL
             ("<report"
              " id=\"%s\""
              " format_id=\"%s\""
              " extension=\"%s\""
              " content_type=\"%s\">",
              report_iterator_uuid (&reports),
              get_reports_data->format_id,
              extension,
              content_type);

            g_free (extension);
            g_free (content_type);

            if (output && strlen (output))
              {
                /* Encode and send the output. */

                if (strcmp (get_reports_data->format_id,
                            "d5da9f67-8551-4e51-807b-b6a873d70e34"))
                  {
                    gchar *base64;
                    base64 = g_base64_encode ((guchar*) output, output_len);
                    if (send_to_client (base64,
                                        write_to_client,
                                        write_to_client_data))
                      {
                        g_free (output);
                        g_free (base64);
                        cleanup_iterator (&reports);
                        error_send_to_client (error);
                        return;
                      }
                    g_free (base64);
                  }
                else
                  {
                    /* Special case the XML report, bah. */
                    if (send_to_client (output,
                                        write_to_client,
                                        write_to_client_data))
                      {
                        g_free (output);
                        cleanup_iterator (&reports);
                        error_send_to_client (error);
                        return;
                      }
                  }
              }
            g_free (output);
            SEND_TO_CLIENT_OR_FAIL ("</report>");
          }
        cleanup_iterator (&reports);
        SEND_TO_CLIENT_OR_FAIL ("</get_reports_response>");

        get_reports_data_reset (get_reports_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_GET_REPORT_FORMATS:
        {
          report_format_t report_format = 0;

          assert (strcasecmp ("GET_REPORT_FORMATS", element_name) == 0);

          if (get_report_formats_data->report_format_id
              && find_report_format (get_report_formats_data->report_format_id,
                                     &report_format))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_report_formats"));
          else if (get_report_formats_data->report_format_id
                   && report_format == 0)
            {
              if (send_find_error_to_client
                   ("get_report_formats",
                    "report_format",
                    get_report_formats_data->report_format_id,
                    write_to_client,
                    write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              iterator_t report_formats;

              SEND_TO_CLIENT_OR_FAIL ("<get_report_formats_response"
                                      " status=\"" STATUS_OK "\""
                                      " status_text=\"" STATUS_OK_TEXT "\">");
              init_report_format_iterator (&report_formats,
                                           report_format,
                                           get_report_formats_data->sort_order,
                                           get_report_formats_data->sort_field);
              while (next (&report_formats))
                {
                  time_t trust_time;

                  trust_time = report_format_iterator_trust_time
                                (&report_formats);

                  SENDF_TO_CLIENT_OR_FAIL
                   ("<report_format id=\"%s\">"
                    "<name>%s</name>"
                    "<extension>%s</extension>"
                    "<content_type>%s</content_type>"
                    "<summary>%s</summary>"
                    "<description>%s</description>"
                    "<global>%i</global>",
                    report_format_iterator_uuid (&report_formats),
                    report_format_iterator_name (&report_formats),
                    report_format_iterator_extension (&report_formats),
                    report_format_iterator_content_type (&report_formats),
                    report_format_iterator_summary (&report_formats),
                    report_format_iterator_description (&report_formats),
                    report_format_iterator_global (&report_formats));

                  if (get_report_formats_data->params
                      || get_report_formats_data->export)
                    {
                      iterator_t params;
                      init_report_format_param_iterator
                       (&params,
                        report_format_iterator_report_format (&report_formats),
                        1,
                        NULL);
                      while (next (&params))
                        SENDF_TO_CLIENT_OR_FAIL
                         ("<param><name>%s</name><value>%s</value></param>",
                          report_format_param_iterator_name (&params),
                          report_format_param_iterator_value (&params));
                      cleanup_iterator (&params);
                    }

                  if (get_report_formats_data->export)
                    {
                      file_iterator_t files;
                      init_report_format_file_iterator
                       (&files,
                        report_format_iterator_report_format (&report_formats));
                      while (next_file (&files))
                        {
                          gchar *content = file_iterator_content_64 (&files);
                          SENDF_TO_CLIENT_OR_FAIL
                           ("<file name=\"%s\">%s</file>",
                            file_iterator_name (&files),
                            content);
                          g_free (content);
                        }
                      cleanup_file_iterator (&files);

                      SENDF_TO_CLIENT_OR_FAIL
                       ("<signature>%s</signature>",
                        report_format_iterator_signature (&report_formats));
                    }
                  else
                    SENDF_TO_CLIENT_OR_FAIL
                     ("<trust>%s<time>%s</time></trust>"
                      "<active>%i</active>",
                      report_format_iterator_trust (&report_formats),
                      ctime_strip_newline (&trust_time),
                      report_format_iterator_active (&report_formats));

                  SEND_TO_CLIENT_OR_FAIL ("</report_format>");
                }
              cleanup_iterator (&report_formats);
              SEND_TO_CLIENT_OR_FAIL ("</get_report_formats_response>");
            }
          get_report_formats_data_reset (get_report_formats_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_TARGET_LOCATORS:
        {
          assert (strcasecmp ("GET_TARGET_LOCATORS", element_name) == 0);
          GSList* sources = resource_request_sources (RESOURCE_TYPE_TARGET);
          GSList* source = sources;

          SEND_TO_CLIENT_OR_FAIL ("<get_target_locators_response"
                                  " status=\"" STATUS_OK "\""
                                  " status_text=\"" STATUS_OK_TEXT "\">");

          while (source)
            {
              SENDF_TO_CLIENT_OR_FAIL ("<target_locator>"
                                       "<name>%s</name>"
                                       "</target_locator>",
                                       (char*) source->data);
              source = g_slist_next (source);
            }

          SEND_TO_CLIENT_OR_FAIL ("</get_target_locators_response>");

          /* Clean up. */
          openvas_string_list_free (sources);

          set_client_state (CLIENT_AUTHENTIC);

          break;
        }

      case CLIENT_GET_RESULTS:
        {
          result_t result = 0;
          task_t task = 0;

          assert (strcasecmp ("GET_RESULTS", element_name) == 0);

          if (current_credentials.username == NULL)
            {
              get_results_data_reset (get_results_data);
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_results"));
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }

          if (get_results_data->notes
              && (get_results_data->task_id == NULL))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_results",
                                "GET_RESULTS must have a task_id attribute"
                                " if the notes attribute is true"));
          else if ((get_results_data->overrides
                    || get_results_data->apply_overrides)
                   && (get_results_data->task_id == NULL))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_results",
                                "GET_RESULTS must have a task_id attribute"
                                " if either of the overrides attributes is"
                                " true"));
          else if (get_results_data->result_id
                   && find_result (get_results_data->result_id, &result))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_results"));
          else if (get_results_data->result_id && result == 0)
            {
              if (send_find_error_to_client ("get_results",
                                             "result",
                                             get_results_data->result_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (get_results_data->task_id
                   && find_task (get_results_data->task_id, &task))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_results"));
          else if (get_results_data->task_id && task == 0)
            {
              if (send_find_error_to_client ("get_results",
                                             "task",
                                             get_results_data->task_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              SEND_TO_CLIENT_OR_FAIL ("<get_results_response"
                                      " status=\"" STATUS_OK "\""
                                      " status_text=\"" STATUS_OK_TEXT "\">"
                                      "<results>");
              init_result_iterator (&results, 0, result, NULL, 0, 1, 1, NULL,
                                    NULL, NULL, NULL,
                                    get_results_data->apply_overrides);
              while (next (&results))
                {
                  GString *buffer = g_string_new ("");
                  buffer_results_xml (buffer,
                                      &results,
                                      task,
                                      get_results_data->notes,
                                      get_results_data->notes_details,
                                      get_results_data->overrides,
                                      get_results_data->overrides_details);
                  SEND_TO_CLIENT_OR_FAIL (buffer->str);
                  g_string_free (buffer, TRUE);
                }
              cleanup_iterator (&results);
              SEND_TO_CLIENT_OR_FAIL ("</results>"
                                      "</get_results_response>");
            }

          get_results_data_reset (get_results_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_VERSION:
      case CLIENT_GET_VERSION_AUTHENTIC:
        SEND_TO_CLIENT_OR_FAIL ("<get_version_response"
                                " status=\"" STATUS_OK "\""
                                " status_text=\"" STATUS_OK_TEXT "\">"
                                "<version>1.0</version>"
                                "</get_version_response>");
        if (client_state)
          set_client_state (CLIENT_AUTHENTIC);
        else
          set_client_state (CLIENT_TOP);
        break;

      case CLIENT_GET_SCHEDULES:
        {
          schedule_t schedule = 0;

          assert (strcasecmp ("GET_SCHEDULES", element_name) == 0);

          if (get_schedules_data->schedule_id
              && find_schedule (get_schedules_data->schedule_id, &schedule))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_schedules"));
          else if (get_schedules_data->schedule_id && schedule == 0)
            {
              if (send_find_error_to_client ("get_schedules",
                                             "schedule",
                                             get_schedules_data->schedule_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              iterator_t schedules;
              GString *buffer;

              SENDF_TO_CLIENT_OR_FAIL ("<get_schedules_response"
                                       " status=\"" STATUS_OK "\""
                                       " status_text=\"" STATUS_OK_TEXT "\">");

              buffer = g_string_new ("");

              init_schedule_iterator (&schedules,
                                      schedule,
                                      get_schedules_data->sort_order,
                                      get_schedules_data->sort_field);
              buffer_schedules_xml (buffer, &schedules, get_schedules_data->details
                                    /* get_schedules_data->tasks */);
              cleanup_iterator (&schedules);

              SEND_TO_CLIENT_OR_FAIL (buffer->str);
              g_string_free (buffer, TRUE);

              SEND_TO_CLIENT_OR_FAIL ("</get_schedules_response>");
            }

          get_schedules_data_reset (get_schedules_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_DELETE_AGENT:
        assert (strcasecmp ("DELETE_AGENT", element_name) == 0);
        if (delete_agent_data->agent_id)
          {
            agent_t agent;

            if (find_agent (delete_agent_data->agent_id, &agent))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_agent"));
            else if (agent == 0)
              {
                if (send_find_error_to_client ("delete_agent",
                                               "agent",
                                               delete_agent_data->agent_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (delete_agent (agent))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_agent"));
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("delete_agent",
                                      "Agent is in use"));
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("delete_agent"));
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_agent",
                              "DELETE_AGENT requires an agent_id"
                              " attribute"));
        delete_agent_data_reset (delete_agent_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_CONFIG:
        assert (strcasecmp ("DELETE_CONFIG", element_name) == 0);
        if (delete_config_data->config_id)
          {
            config_t config = 0;

            if (find_config (delete_config_data->config_id, &config))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_config"));
            else if (config == 0)
              {
                if (send_find_error_to_client ("delete_config",
                                               "config",
                                               delete_config_data->config_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (delete_config (config))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_config"));
                  g_log ("event config", G_LOG_LEVEL_MESSAGE,
                         "Scan config %s has been deleted",
                         delete_config_data->config_id);
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL (XML_ERROR_SYNTAX ("delete_config",
                                                            "Config is in use"));
                  g_log ("event config", G_LOG_LEVEL_MESSAGE,
                         "Scan config %s could not be deleted",
                         delete_config_data->config_id);
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_config"));
                  g_log ("event config", G_LOG_LEVEL_MESSAGE,
                         "Scan config %s could not be deleted",
                         delete_config_data->config_id);
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_config",
                              "DELETE_CONFIG requires a config_id attribute"));
        delete_config_data_reset (delete_config_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_ESCALATOR:
        assert (strcasecmp ("DELETE_ESCALATOR", element_name) == 0);
        if (delete_escalator_data->escalator_id)
          {
            escalator_t escalator;

            if (find_escalator (delete_escalator_data->escalator_id,
                                &escalator))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_escalator"));
            else if (escalator == 0)
              {
                if (send_find_error_to_client
                     ("delete_escalator",
                      "escalator",
                      delete_escalator_data->escalator_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (delete_escalator (escalator))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_escalator"));
                  g_log ("event escalator", G_LOG_LEVEL_MESSAGE,
                         "Escalator %s has been deleted",
                         delete_escalator_data->escalator_id);
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("delete_escalator",
                                      "Escalator is in use"));
                  g_log ("event escalator", G_LOG_LEVEL_MESSAGE,
                         "Escalator %s could not be deleted",
                         delete_escalator_data->escalator_id);
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("delete_escalator"));
                  g_log ("event escalator", G_LOG_LEVEL_MESSAGE,
                         "Escalator %s could not be deleted",
                         delete_escalator_data->escalator_id);
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_escalator",
                              "DELETE_ESCALATOR requires an escalator_id"
                              " attribute"));
        delete_escalator_data_reset (delete_escalator_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_LSC_CREDENTIAL:
        assert (strcasecmp ("DELETE_LSC_CREDENTIAL", element_name) == 0);
        if (delete_lsc_credential_data->lsc_credential_id)
          {
            lsc_credential_t lsc_credential = 0;

            if (find_lsc_credential
                 (delete_lsc_credential_data->lsc_credential_id,
                  &lsc_credential))
              SEND_TO_CLIENT_OR_FAIL
               (XML_INTERNAL_ERROR ("delete_lsc_credential"));
            else if (lsc_credential == 0)
              {
                if (send_find_error_to_client
                     ("delete_lsc_credential",
                      "LSC credential",
                      delete_lsc_credential_data->lsc_credential_id,
                      write_to_client,
                      write_to_client_data))

                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (delete_lsc_credential (lsc_credential))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_lsc_credential"));
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("delete_lsc_credential",
                                      "LSC credential is in use"));
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("delete_lsc_credential"));
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_lsc_credential",
                              "DELETE_LSC_CREDENTIAL requires an"
                              " lsc_credential_id attribute"));
        delete_lsc_credential_data_reset (delete_lsc_credential_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_SLAVE:
        assert (strcasecmp ("DELETE_SLAVE", element_name) == 0);
        if (delete_slave_data->slave_id)
          {
            slave_t slave = 0;

            if (find_slave (delete_slave_data->slave_id, &slave))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_slave"));
            else if (slave == 0)
              {
                if (send_find_error_to_client ("delete_slave",
                                               "slave",
                                               delete_slave_data->slave_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (delete_slave (slave))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_slave"));
                  g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                         "Slave %s has been deleted",
                         delete_slave_data->slave_id);
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL (XML_ERROR_SYNTAX ("delete_slave",
                                                            "Slave is in use"));
                  g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                         "Slave %s could not be deleted",
                         delete_slave_data->slave_id);
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_slave"));
                  g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                         "Slave %s could not be deleted",
                         delete_slave_data->slave_id);
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_slave",
                              "DELETE_SLAVE requires a slave_id attribute"));
        delete_slave_data_reset (delete_slave_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_TARGET:
        assert (strcasecmp ("DELETE_TARGET", element_name) == 0);
        if (delete_target_data->target_id)
          {
            target_t target = 0;

            if (find_target (delete_target_data->target_id, &target))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_target"));
            else if (target == 0)
              {
                if (send_find_error_to_client ("delete_target",
                                               "target",
                                               delete_target_data->target_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (delete_target (target))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_target"));
                  g_log ("event target", G_LOG_LEVEL_MESSAGE,
                         "Target %s has been deleted",
                         delete_target_data->target_id);
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL (XML_ERROR_SYNTAX ("delete_target",
                                                            "Target is in use"));
                  g_log ("event target", G_LOG_LEVEL_MESSAGE,
                         "Target %s could not be deleted",
                         delete_target_data->target_id);
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_target"));
                  g_log ("event target", G_LOG_LEVEL_MESSAGE,
                         "Target %s could not be deleted",
                         delete_target_data->target_id);
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_target",
                              "DELETE_TARGET requires a target_id attribute"));
        delete_target_data_reset (delete_target_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_TASK:
        if (delete_task_data->task_id)
          {
            task_t task;
            if (find_task (delete_task_data->task_id, &task))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_task"));
            else if (task == 0)
              {
                if (send_find_error_to_client ("delete_task",
                                               "task",
                                               delete_task_data->task_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (request_delete_task (&task))
              {
                case 0:    /* Deleted. */
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Task %s has been deleted",
                         delete_task_data->task_id);
                  break;
                case 1:    /* Delete requested. */
                  SEND_TO_CLIENT_OR_FAIL (XML_OK_REQUESTED ("delete_task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Deletion of task %s has been requested",
                         delete_task_data->task_id);
                  break;
                case 2:    /* Hidden task. */
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("delete_task",
                                      "Attempt to delete a hidden task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Task %s could not be deleted",
                         delete_task_data->task_id);
                  break;
                default:   /* Programming error. */
                  assert (0);
                case -1:
                  /* to_scanner is full. */
                  /** @todo Or some other error occurred. */
                  /** @todo Consider reverting parsing for retry. */
                  /** @todo process_omp_client_input must return -2. */
                  tracef ("delete_task failed\n");
                  abort ();
                  break;
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_task",
                              "DELETE_TASK requires a task_id attribute"));
        delete_task_data_reset (delete_task_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_HELP:
        SEND_TO_CLIENT_OR_FAIL ("<help_response"
                                " status=\"" STATUS_OK "\""
                                " status_text=\"" STATUS_OK_TEXT "\">");
        SEND_TO_CLIENT_OR_FAIL (help_text);
        SEND_TO_CLIENT_OR_FAIL ("</help_response>");
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_MODIFY_CONFIG:
        {
          config_t config;
          if (modify_config_data->config_id == NULL
              || strlen (modify_config_data->config_id) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_config",
                                "MODIFY_CONFIG requires a config_id"
                                " attribute"));
          else if ((modify_config_data->nvt_selection_family
                    /* This array implies FAMILY_SELECTION. */
                    && modify_config_data->families_static_all)
                   || ((modify_config_data->nvt_selection_family
                        || modify_config_data->families_static_all)
                       && (modify_config_data->preference_name
                           || modify_config_data->preference_value
                           || modify_config_data->preference_nvt_oid)))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_config",
                                "MODIFY_CONFIG requires either a PREFERENCE or"
                                " an NVT_SELECTION or a FAMILY_SELECTION"));
          else if (find_config (modify_config_data->config_id, &config))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_config"));
          else if (config == 0)
            {
              if (send_find_error_to_client ("modify_config",
                                             "config",
                                             modify_config_data->config_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (modify_config_data->nvt_selection_family)
            {
              assert (modify_config_data->nvt_selection);

              array_terminate (modify_config_data->nvt_selection);
              switch (manage_set_config_nvts
                       (config,
                        modify_config_data->nvt_selection_family,
                        modify_config_data->nvt_selection))
                {
                  case 0:
                    SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_config"));
                    g_log ("event config", G_LOG_LEVEL_MESSAGE,
                           "Scan config %s has been modified",
                           modify_config_data->config_id);
                    break;
                  case 1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("modify_config", "Config is in use"));
                    g_log ("event config", G_LOG_LEVEL_MESSAGE,
                           "Scan config %s could not be modified",
                           modify_config_data->config_id);
                    break;
#if 0
                  case -1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("modify_config",
                                        "MODIFY_CONFIG PREFERENCE requires at"
                                        " least one of the VALUE and NVT"
                                        " elements"));
                    break;
#endif
                  default:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("modify_config"));
                    g_log ("event config", G_LOG_LEVEL_MESSAGE,
                           "Scan config %s could not be modified",
                           modify_config_data->config_id);
                    break;
                }
            }
          else if (modify_config_data->families_static_all)
            {
              /* There was a FAMILY_SELECTION. */

              assert (modify_config_data->families_growing_all);
              assert (modify_config_data->families_static_all);

              array_terminate (modify_config_data->families_growing_all);
              array_terminate (modify_config_data->families_static_all);
              array_terminate (modify_config_data->families_growing_empty);
              switch (manage_set_config_families
                       (config,
                        modify_config_data->families_growing_all,
                        modify_config_data->families_static_all,
                        modify_config_data->families_growing_empty,
                        modify_config_data->family_selection_growing))
                {
                  case 0:
                    SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_config"));
                    g_log ("event config", G_LOG_LEVEL_MESSAGE,
                           "Scan config %s has been modified",
                           modify_config_data->config_id);
                    break;
                  case 1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("modify_config", "Config is in use"));
                    g_log ("event config", G_LOG_LEVEL_MESSAGE,
                           "Scan config %s could not be modified",
                           modify_config_data->config_id);
                    break;
#if 0
                  case -1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("modify_config",
                                        "MODIFY_CONFIG PREFERENCE requires at"
                                        " least one of the VALUE and NVT"
                                        " elements"));
                    break;
#endif
                  default:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("modify_config"));
                    g_log ("event config", G_LOG_LEVEL_MESSAGE,
                           "Scan config %s could not be modified",
                           modify_config_data->config_id);
                    break;
                }
            }
          else if (modify_config_data->preference_name == NULL
                   || strlen (modify_config_data->preference_name) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_config",
                                "MODIFY_CONFIG PREFERENCE requires a NAME"
                                " element"));
          else switch (manage_set_config_preference
                        (config,
                         modify_config_data->preference_nvt_oid,
                         modify_config_data->preference_name,
                         modify_config_data->preference_value))
            {
              case 0:
                SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_config"));
                break;
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_config", "Config is in use"));
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_config", "Empty radio value"));
                break;
              case -1:
                SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_config"));
                break;
              default:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("modify_config"));
                break;
            }
        }
        modify_config_data_reset (modify_config_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;
      case CLIENT_MODIFY_CONFIG_FAMILY_SELECTION:
        assert (strcasecmp ("FAMILY_SELECTION", element_name) == 0);
        set_client_state (CLIENT_MODIFY_CONFIG);
        break;
      case CLIENT_MODIFY_CONFIG_NVT_SELECTION:
        assert (strcasecmp ("NVT_SELECTION", element_name) == 0);
        set_client_state (CLIENT_MODIFY_CONFIG);
        break;
      case CLIENT_MODIFY_CONFIG_PREFERENCE:
        assert (strcasecmp ("PREFERENCE", element_name) == 0);
        set_client_state (CLIENT_MODIFY_CONFIG);
        break;

      case CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY:
        assert (strcasecmp ("FAMILY", element_name) == 0);
        if (modify_config_data->family_selection_family_name)
          {
            if (modify_config_data->family_selection_family_growing)
              {
                if (modify_config_data->family_selection_family_all)
                  /* Growing 1 and select all 1. */
                  array_add (modify_config_data->families_growing_all,
                             modify_config_data->family_selection_family_name);
                else
                  /* Growing 1 and select all 0. */
                  array_add (modify_config_data->families_growing_empty,
                             modify_config_data->family_selection_family_name);
              }
            else
              {
                if (modify_config_data->family_selection_family_all)
                  /* Growing 0 and select all 1. */
                  array_add (modify_config_data->families_static_all,
                             modify_config_data->family_selection_family_name);
                /* Else growing 0 and select all 0. */
              }
          }
        modify_config_data->family_selection_family_name = NULL;
        set_client_state (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION);
        break;
      case CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_GROWING:
        assert (strcasecmp ("GROWING", element_name) == 0);
        if (modify_config_data->family_selection_growing_text)
          {
            modify_config_data->family_selection_growing
             = atoi (modify_config_data->family_selection_growing_text);
            openvas_free_string_var
             (&modify_config_data->family_selection_growing_text);
          }
        else
          modify_config_data->family_selection_growing = 0;
        set_client_state (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION);
        break;

      case CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_ALL:
        assert (strcasecmp ("ALL", element_name) == 0);
        if (modify_config_data->family_selection_family_all_text)
          {
            modify_config_data->family_selection_family_all
             = atoi (modify_config_data->family_selection_family_all_text);
            openvas_free_string_var
             (&modify_config_data->family_selection_family_all_text);
          }
        else
          modify_config_data->family_selection_family_all = 0;
        set_client_state (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY);
        break;
      case CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY);
        break;
      case CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_GROWING:
        assert (strcasecmp ("GROWING", element_name) == 0);
        if (modify_config_data->family_selection_family_growing_text)
          {
            modify_config_data->family_selection_family_growing
             = atoi (modify_config_data->family_selection_family_growing_text);
            openvas_free_string_var
             (&modify_config_data->family_selection_family_growing_text);
          }
        else
          modify_config_data->family_selection_family_growing = 0;
        set_client_state (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY);
        break;

      case CLIENT_MODIFY_CONFIG_NVT_SELECTION_FAMILY:
        assert (strcasecmp ("FAMILY", element_name) == 0);
        set_client_state (CLIENT_MODIFY_CONFIG_NVT_SELECTION);
        break;
      case CLIENT_MODIFY_CONFIG_NVT_SELECTION_NVT:
        assert (strcasecmp ("NVT", element_name) == 0);
        if (modify_config_data->nvt_selection_nvt_oid)
          array_add (modify_config_data->nvt_selection,
                     modify_config_data->nvt_selection_nvt_oid);
        modify_config_data->nvt_selection_nvt_oid = NULL;
        set_client_state (CLIENT_MODIFY_CONFIG_NVT_SELECTION);
        break;

      case CLIENT_MODIFY_CONFIG_PREFERENCE_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_MODIFY_CONFIG_PREFERENCE);
        break;
      case CLIENT_MODIFY_CONFIG_PREFERENCE_NVT:
        assert (strcasecmp ("NVT", element_name) == 0);
        set_client_state (CLIENT_MODIFY_CONFIG_PREFERENCE);
        break;
      case CLIENT_MODIFY_CONFIG_PREFERENCE_VALUE:
        assert (strcasecmp ("VALUE", element_name) == 0);
        /* Init, so it's the empty string when the value is empty. */
        openvas_append_string (&modify_config_data->preference_value, "");
        set_client_state (CLIENT_MODIFY_CONFIG_PREFERENCE);
        break;

      case CLIENT_MODIFY_REPORT:
        {
          report_t report = 0;

          if (modify_report_data->report_id == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_report",
                                "MODIFY_REPORT requires a report_id attribute"));
          else if (modify_report_data->comment == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_report",
                                "MODIFY_REPORT requires a COMMENT element"));
          else if (find_report (modify_report_data->report_id, &report))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_report"));
          else if (report == 0)
            {
              if (send_find_error_to_client ("modify_report",
                                             "report",
                                             modify_report_data->report_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              int ret = set_report_parameter
                         (report,
                          "COMMENT",
                          modify_report_data->comment);
              switch (ret)
                {
                  case 0:
                    SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_report"));
                    break;
                  case -2: /* Parameter name error. */
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("modify_report",
                                        "Bogus MODIFY_REPORT parameter"));
                    break;
                  case -3: /* Failed to write to disk. */
                  default:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("modify_report"));
                    break;
                }
            }
          SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_report"));
        }
        modify_report_data_reset (modify_report_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;
      case CLIENT_MODIFY_REPORT_COMMENT:
        assert (strcasecmp ("COMMENT", element_name) == 0);
        set_client_state (CLIENT_MODIFY_REPORT);
        break;

      case CLIENT_MODIFY_REPORT_FORMAT:
        {
          report_format_t report_format = 0;

          if (modify_report_format_data->report_format_id == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_report_format",
                                "MODIFY_REPORT_FORMAT requires a"
                                " report_format_id attribute"));
          else if (find_report_format
                    (modify_report_format_data->report_format_id,
                     &report_format))
            SEND_TO_CLIENT_OR_FAIL
             (XML_INTERNAL_ERROR ("modify_report_format"));
          else if (report_format == 0)
            {
              if (send_find_error_to_client
                   ("modify_report_format",
                    "report format",
                    modify_report_format_data->report_format_id,
                    write_to_client,
                    write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              if (modify_report_format_data->name)
                set_report_format_name (report_format,
                                        modify_report_format_data->name);
              if (modify_report_format_data->summary)
                set_report_format_summary (report_format,
                                           modify_report_format_data->summary);
              SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_report_format"));
            }
        }
        modify_report_format_data_reset (modify_report_format_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;
      case CLIENT_MODIFY_REPORT_FORMAT_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_MODIFY_REPORT_FORMAT);
        break;
      case CLIENT_MODIFY_REPORT_FORMAT_SUMMARY:
        assert (strcasecmp ("SUMMARY", element_name) == 0);
        set_client_state (CLIENT_MODIFY_REPORT_FORMAT);
        break;

      case CLIENT_MODIFY_TASK:
        /** @todo Update to match "create_task (config, target)". */
        if (modify_task_data->task_id)
          {
            task_t task;
            if (find_task (modify_task_data->task_id, &task))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_task"));
            else if (task == 0)
              {
                if (send_find_error_to_client ("modify_task",
                                               "task",
                                               modify_task_data->task_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else if ((modify_task_data->action
                      || modify_task_data->escalator_id
                      || modify_task_data->name
                      || modify_task_data->rcfile)
                     == 0)
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("modify_task",
                                  "Too few parameters"));
            else if (modify_task_data->action
                     && (modify_task_data->comment
                         || modify_task_data->escalator_id
                         || modify_task_data->name
                         || modify_task_data->rcfile))
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("modify_task",
                                  "Too many parameters at once"));
            else if (modify_task_data->action)
              {
                if (modify_task_data->file_name == NULL)
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("modify_task",
                                      "MODIFY_TASK FILE requires a name"
                                      " attribute"));
                else if (strcmp (modify_task_data->action, "update") == 0)
                  {
                    manage_task_update_file (task,
                                             modify_task_data->file_name,
                                             modify_task_data->file
                                              ? modify_task_data->file
                                              : "");
                    g_log ("event task", G_LOG_LEVEL_MESSAGE,
                           "Task %s has been modified",
                           modify_task_data->task_id);
                    SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_task"));
                  }
                else if (strcmp (modify_task_data->action, "remove") == 0)
                  {
                    manage_task_remove_file (task, modify_task_data->file_name);
                    g_log ("event task", G_LOG_LEVEL_MESSAGE,
                           "Task %s has been modified",
                           modify_task_data->task_id);
                    SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_task"));
                  }
                else
                  {
                    SEND_TO_CLIENT_OR_FAIL
                      (XML_ERROR_SYNTAX ("modify_task",
                                         "MODIFY_TASK action must be"
                                         " \"update\" or \"remove\""));
                    g_log ("event task", G_LOG_LEVEL_MESSAGE,
                           "Task %s could not be modified",
                           modify_task_data->task_id);
                  }
              }
            else
              {
                int fail = 0, first = 1;

                /** @todo It'd probably be better to allow only one
                 * modification at a time, that is, one parameter or one of
                 * file, name and comment.  Otherwise a syntax error in a
                 * later part of the command would result in an error being
                 * returned while some part of the command actually
                 * succeeded. */

                if (modify_task_data->rcfile)
                  {
                    fail = set_task_parameter (task,
                                               "RCFILE",
                                               modify_task_data->rcfile);
                    modify_task_data->rcfile = NULL;
                    if (fail)
                      {
                        SEND_TO_CLIENT_OR_FAIL
                          (XML_INTERNAL_ERROR ("modify_task"));
                        g_log ("event task", G_LOG_LEVEL_MESSAGE,
                               "Task %s could not be modified",
                               modify_task_data->task_id);
                      }
                    else
                      first = 0;
                  }

                if (fail == 0 && modify_task_data->name)
                  {
                    fail = set_task_parameter (task,
                                               "NAME",
                                               modify_task_data->name);
                    modify_task_data->name = NULL;
                    if (fail)
                      {
                        SEND_TO_CLIENT_OR_FAIL
                          (XML_INTERNAL_ERROR ("modify_task"));
                        g_log ("event task", G_LOG_LEVEL_MESSAGE,
                               "Task %s could not be modified",
                               modify_task_data->task_id);
                      }
                    else
                      first = 0;
                  }

                if (fail == 0 && modify_task_data->comment)
                  {
                    fail = set_task_parameter (task,
                                               "COMMENT",
                                               modify_task_data->comment);
                    modify_task_data->comment = NULL;
                    if (fail)
                      {
                        SEND_TO_CLIENT_OR_FAIL
                          (XML_INTERNAL_ERROR ("modify_task"));
                        g_log ("event task", G_LOG_LEVEL_MESSAGE,
                               "Task %s could not be modified",
                               modify_task_data->task_id);
                      }
                    else
                      first = 0;
                  }

                if (fail == 0 && modify_task_data->escalator_id)
                  {
                    escalator_t escalator = 0;

                    if (strcmp (modify_task_data->escalator_id, "0") == 0)
                      {
                        set_task_escalator (task, 0);
                        first = 0;
                      }
                    else if ((fail = find_escalator
                                      (modify_task_data->escalator_id,
                                       &escalator)))
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_INTERNAL_ERROR ("modify_task"));
                    else if (escalator == 0)
                      {
                        if (send_find_error_to_client
                             ("modify_task",
                              "escalator",
                              modify_task_data->escalator_id,
                              write_to_client,
                              write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        fail = 1;
                      }
                    else
                      {
                        set_task_escalator (task, escalator);
                        first = 0;
                      }
                  }

                if (fail == 0 && modify_task_data->schedule_id)
                  {
                    schedule_t schedule = 0;

                    if (strcmp (modify_task_data->schedule_id, "0") == 0)
                      {
                        set_task_schedule (task, 0);
                        first = 0;
                      }
                    else if ((fail = find_schedule
                                      (modify_task_data->schedule_id,
                                       &schedule)))
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_INTERNAL_ERROR ("modify_task"));
                    else if (schedule == 0)
                      {
                        if (send_find_error_to_client
                             ("modify_task",
                              "schedule",
                              modify_task_data->schedule_id,
                              write_to_client,
                              write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        fail = 1;
                      }
                    else
                      {
                        set_task_schedule (task, schedule);
                        first = 0;
                      }
                  }

                if (fail == 0)
                  {
                    assert (first == 0);
                    g_log ("event task", G_LOG_LEVEL_MESSAGE,
                           "Task %s has been modified",
                           modify_task_data->task_id);
                    SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_task"));
                  }
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("modify_task",
                              "MODIFY_TASK requires a task_id attribute"));
        modify_task_data_reset (modify_task_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;
      case CLIENT_MODIFY_TASK_COMMENT:
        assert (strcasecmp ("COMMENT", element_name) == 0);
        set_client_state (CLIENT_MODIFY_TASK);
        break;
      case CLIENT_MODIFY_TASK_ESCALATOR:
        assert (strcasecmp ("ESCALATOR", element_name) == 0);
        set_client_state (CLIENT_MODIFY_TASK);
        break;
      case CLIENT_MODIFY_TASK_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_MODIFY_TASK);
        break;
      case CLIENT_MODIFY_TASK_RCFILE:
        assert (strcasecmp ("RCFILE", element_name) == 0);
        set_client_state (CLIENT_MODIFY_TASK);
        break;
      case CLIENT_MODIFY_TASK_SCHEDULE:
        assert (strcasecmp ("SCHEDULE", element_name) == 0);
        set_client_state (CLIENT_MODIFY_TASK);
        break;
      case CLIENT_MODIFY_TASK_FILE:
        assert (strcasecmp ("FILE", element_name) == 0);
        set_client_state (CLIENT_MODIFY_TASK);
        break;

      case CLIENT_CREATE_AGENT:
        {
          agent_t agent;

          assert (strcasecmp ("CREATE_AGENT", element_name) == 0);
          assert (create_agent_data->name != NULL);

          if (strlen (create_agent_data->name) == 0)
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_agent",
                                  "CREATE_AGENT name must be at"
                                  " least one character long"));
            }
          else if (strlen (create_agent_data->installer) == 0)
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_agent",
                                  "CREATE_AGENT installer must be at"
                                  " least one byte long"));
            }
          else switch (create_agent (create_agent_data->name,
                                     create_agent_data->comment,
                                     create_agent_data->installer,
                                     create_agent_data->installer_filename,
                                     create_agent_data->installer_signature,
                                     create_agent_data->howto_install,
                                     create_agent_data->howto_use,
                                     &agent))
            {
              case 0:
                {
                  gchar *uuid;
                  agent_uuid (agent, &uuid);
                  SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_agent"),
                                           uuid);
                  g_free (uuid);
                  break;
                }
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_agent",
                                    "Agent exists already"));
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_agent",
                                    "Name may only contain alphanumeric"
                                    " characters"));
                break;
              default:
                assert (0);
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_agent"));
                break;
            }
          create_agent_data_reset (create_agent_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      case CLIENT_CREATE_AGENT_COMMENT:
        assert (strcasecmp ("COMMENT", element_name) == 0);
        set_client_state (CLIENT_CREATE_AGENT);
        break;
      case CLIENT_CREATE_AGENT_HOWTO_INSTALL:
        assert (strcasecmp ("HOWTO_INSTALL", element_name) == 0);
        set_client_state (CLIENT_CREATE_AGENT);
        break;
      case CLIENT_CREATE_AGENT_HOWTO_USE:
        assert (strcasecmp ("HOWTO_USE", element_name) == 0);
        set_client_state (CLIENT_CREATE_AGENT);
        break;
      case CLIENT_CREATE_AGENT_INSTALLER:
        assert (strcasecmp ("INSTALLER", element_name) == 0);
        set_client_state (CLIENT_CREATE_AGENT);
        break;
      case CLIENT_CREATE_AGENT_INSTALLER_FILENAME:
        assert (strcasecmp ("FILENAME", element_name) == 0);
        set_client_state (CLIENT_CREATE_AGENT_INSTALLER);
        break;
      case CLIENT_CREATE_AGENT_INSTALLER_SIGNATURE:
        assert (strcasecmp ("SIGNATURE", element_name) == 0);
        set_client_state (CLIENT_CREATE_AGENT_INSTALLER);
        break;
      case CLIENT_CREATE_AGENT_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_CREATE_AGENT);
        break;

      case CLIENT_CREATE_CONFIG:
        {
          config_t config = 0, new_config;

          assert (strcasecmp ("CREATE_CONFIG", element_name) == 0);
          assert (import_config_data->import
                  || (create_config_data->name != NULL));

          /* For now the import element, GET_CONFIGS_RESPONSE, overrides
           * any other elements. */
          if (import_config_data->import)
            {
              char *name;
              array_terminate (import_config_data->nvt_selectors);
              array_terminate (import_config_data->preferences);
              switch (create_config (import_config_data->name,
                                     import_config_data->comment,
                                     import_config_data->nvt_selectors,
                                     import_config_data->preferences,
                                     &new_config,
                                     &name))
                {
                  case 0:
                    {
                      gchar *uuid;
                      config_uuid (new_config, &uuid);
                      SENDF_TO_CLIENT_OR_FAIL
                       ("<create_config_response"
                        " status=\"" STATUS_OK_CREATED "\""
                        " status_text=\"" STATUS_OK_CREATED_TEXT "\""
                        " id=\"%s\">"
                        /* This is a hack for the GSA, which should really
                         * do a GET_CONFIG with the ID to get the name. */
                        "<config id=\"%s\"><name>%s</name></config>"
                        "</create_config_response>",
                        uuid,
                        uuid,
                        name);
                      g_log ("event config", G_LOG_LEVEL_MESSAGE,
                             "Scan config %s has been created", uuid);
                      g_free (uuid);
                      free (name);
                      break;
                    }
                  case 1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_config",
                                        "Config exists already"));
                      g_log ("event config", G_LOG_LEVEL_MESSAGE,
                             "Scan config could not be created");
                    break;
                  case -1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("create_config"));
                      g_log ("event config", G_LOG_LEVEL_MESSAGE,
                             "Scan config could not be created");
                    break;
                  case -2:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_config",
                                        "CREATE_CONFIG import name must be at"
                                        " least one character long"));
                      g_log ("event config", G_LOG_LEVEL_MESSAGE,
                             "Scan config could not be created");
                    break;
                  case -3:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_config",
                                        "Error in NVT_SELECTORS element."));
                      g_log ("event config", G_LOG_LEVEL_MESSAGE,
                             "Scan config could not be created");
                    break;
                  case -4:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_config",
                                        "Error in PREFERENCES element."));
                      g_log ("event config", G_LOG_LEVEL_MESSAGE,
                             "Scan config could not be created");
                    break;
                }
            }
          else if (strlen (create_config_data->name) == 0)
            {
              g_log ("event config", G_LOG_LEVEL_MESSAGE,
                     "Scan config could not be created");
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_config",
                                  /** @todo Legitimate to pass empty rcfile? */
                                  "CREATE_CONFIG name and rcfile must be at"
                                  " least one character long"));
            }
          else if ((create_config_data->rcfile
                    && create_config_data->copy)
                   || (create_config_data->rcfile == NULL
                       && create_config_data->copy == NULL))
            {
              g_log ("event config", G_LOG_LEVEL_MESSAGE,
                     "Scan config could not be created");
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_config",
                                  "CREATE_CONFIG requires either a COPY or an"
                                  " RCFILE element"));
            }
          else if (create_config_data->rcfile)
            {
              int ret;
              gsize base64_len;
              guchar *base64;

              base64 = g_base64_decode (create_config_data->rcfile,
                                        &base64_len);
              /* g_base64_decode can return NULL (Glib 2.12.4-2), at least
               * when create_config_data->rcfile is zero length. */
              if (base64 == NULL)
                {
                  base64 = (guchar*) g_strdup ("");
                  base64_len = 0;
                }

              ret = create_config_rc (create_config_data->name,
                                      create_config_data->comment,
                                      (char*) base64,
                                      &new_config);
              g_free (base64);
              switch (ret)
                {
                  case 0:
                    {
                      char *uuid;
                      config_uuid (new_config, &uuid);
                      SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID
                                                ("create_config"),
                                               uuid);
                      g_log ("event config", G_LOG_LEVEL_MESSAGE,
                             "Scan config %s has been created", uuid);
                      free (uuid);
                      break;
                    }
                  case 1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_config",
                                        "Config exists already"));
                    g_log ("event config", G_LOG_LEVEL_MESSAGE,
                           "Scan config could not be created");
                    break;
                  case -1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("create_config"));
                    g_log ("event config", G_LOG_LEVEL_MESSAGE,
                           "Scan config could not be created");
                    break;
                }
            }
          else if (find_config (create_config_data->copy, &config))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_config"));
          else if (config == 0)
            {
              if (send_find_error_to_client ("create_config",
                                             "config",
                                             create_config_data->copy,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else switch (copy_config (create_config_data->name,
                                    create_config_data->comment,
                                    config,
                                    &new_config))
            {
              case 0:
                {
                  char *uuid;
                  config_uuid (new_config, &uuid);
                  SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_config"),
                                           uuid);
                  g_log ("event config", G_LOG_LEVEL_MESSAGE,
                         "Scan config %s has been created", uuid);
                  free (uuid);
                  break;
                }
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_config",
                                    "Config exists already"));
                g_log ("event config", G_LOG_LEVEL_MESSAGE,
                       "Scan config could not be created");
                break;
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_config"));
                g_log ("event config", G_LOG_LEVEL_MESSAGE,
                       "Scan config could not be created");
                break;
            }
          create_config_data_reset (create_config_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      case CLIENT_CREATE_CONFIG_COMMENT:
        assert (strcasecmp ("COMMENT", element_name) == 0);
        set_client_state (CLIENT_CREATE_CONFIG);
        break;
      case CLIENT_CREATE_CONFIG_COPY:
        assert (strcasecmp ("COPY", element_name) == 0);
        set_client_state (CLIENT_CREATE_CONFIG);
        break;
      case CLIENT_CREATE_CONFIG_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_CREATE_CONFIG);
        break;
      case CLIENT_CREATE_CONFIG_RCFILE:
        assert (strcasecmp ("RCFILE", element_name) == 0);
        set_client_state (CLIENT_CREATE_CONFIG);
        break;

      case CLIENT_C_C_GCR:
        assert (strcasecmp ("GET_CONFIGS_RESPONSE", element_name) == 0);
        import_config_data->import = 1;
        set_client_state (CLIENT_CREATE_CONFIG);
        break;
      case CLIENT_C_C_GCR_CONFIG:
        assert (strcasecmp ("CONFIG", element_name) == 0);
        set_client_state (CLIENT_C_C_GCR);
        break;
      case CLIENT_C_C_GCR_CONFIG_COMMENT:
        assert (strcasecmp ("COMMENT", element_name) == 0);
        set_client_state (CLIENT_C_C_GCR_CONFIG);
        break;
      case CLIENT_C_C_GCR_CONFIG_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_C_C_GCR_CONFIG);
        break;
      case CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS:
        assert (strcasecmp ("NVT_SELECTORS", element_name) == 0);
        set_client_state (CLIENT_C_C_GCR_CONFIG);
        break;
      case CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR:
        {
          int include;

          assert (strcasecmp ("NVT_SELECTOR", element_name) == 0);

          if (import_config_data->nvt_selector_include
              && strcmp (import_config_data->nvt_selector_include, "0") == 0)
            include = 0;
          else
            include = 1;

          array_add (import_config_data->nvt_selectors,
                     nvt_selector_new
                      (import_config_data->nvt_selector_name,
                       import_config_data->nvt_selector_type,
                       include,
                       import_config_data->nvt_selector_family_or_nvt));

          import_config_data->nvt_selector_name = NULL;
          import_config_data->nvt_selector_type = NULL;
          free (import_config_data->nvt_selector_include);
          import_config_data->nvt_selector_include = NULL;
          import_config_data->nvt_selector_family_or_nvt = NULL;

          set_client_state (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS);
          break;
        }
      case CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_INCLUDE:
        assert (strcasecmp ("INCLUDE", element_name) == 0);
        set_client_state (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR);
        break;
      case CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR);
        break;
      case CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_TYPE:
        assert (strcasecmp ("TYPE", element_name) == 0);
        set_client_state (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR);
        break;
      case CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_FAMILY_OR_NVT:
        assert (strcasecmp ("FAMILY_OR_NVT", element_name) == 0);
        set_client_state (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR);
        break;
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES:
        assert (strcasecmp ("PREFERENCES", element_name) == 0);
        set_client_state (CLIENT_C_C_GCR_CONFIG);
        break;
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE:
        assert (strcasecmp ("PREFERENCE", element_name) == 0);
        array_terminate (import_config_data->preference_alts);
        array_add (import_config_data->preferences,
                   preference_new (import_config_data->preference_name,
                                   import_config_data->preference_type,
                                   import_config_data->preference_value,
                                   import_config_data->preference_nvt_name,
                                   import_config_data->preference_nvt_oid,
                                   import_config_data->preference_alts));
        import_config_data->preference_name = NULL;
        import_config_data->preference_type = NULL;
        import_config_data->preference_value = NULL;
        import_config_data->preference_nvt_name = NULL;
        import_config_data->preference_nvt_oid = NULL;
        import_config_data->preference_alts = NULL;
        set_client_state (CLIENT_C_C_GCR_CONFIG_PREFERENCES);
        break;
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_ALT:
        assert (strcasecmp ("ALT", element_name) == 0);
        array_add (import_config_data->preference_alts,
                   import_config_data->preference_alt);
        import_config_data->preference_alt = NULL;
        set_client_state (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE);
        break;
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE);
        break;
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NVT:
        assert (strcasecmp ("NVT", element_name) == 0);
        set_client_state (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE);
        break;
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NVT_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NVT);
        break;
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_TYPE:
        assert (strcasecmp ("TYPE", element_name) == 0);
        set_client_state (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE);
        break;
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_VALUE:
        assert (strcasecmp ("VALUE", element_name) == 0);
        set_client_state (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE);
        break;

      case CLIENT_CREATE_ESCALATOR:
        {
          event_t event;
          escalator_condition_t condition;
          escalator_method_t method;
          escalator_t new_escalator;

          assert (strcasecmp ("CREATE_ESCALATOR", element_name) == 0);
          assert (create_escalator_data->name != NULL);
          assert (create_escalator_data->condition != NULL);
          assert (create_escalator_data->method != NULL);
          assert (create_escalator_data->event != NULL);

          array_terminate (create_escalator_data->condition_data);
          array_terminate (create_escalator_data->event_data);
          array_terminate (create_escalator_data->method_data);

          if (strlen (create_escalator_data->name) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_escalator",
                                "CREATE_ESCALATOR requires NAME element which"
                                " is at least one character long"));
          else if (strlen (create_escalator_data->condition) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_escalator",
                                "CREATE_ESCALATOR requires a value in a"
                                " CONDITION element"));
          else if (strlen (create_escalator_data->event) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_escalator",
                                "CREATE_ESCALATOR requires a value in an"
                                " EVENT element"));
          else if (strlen (create_escalator_data->method) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_escalator",
                                "CREATE_ESCALATOR requires a value in a"
                                " METHOD element"));
          else if ((condition = escalator_condition_from_name
                                 (create_escalator_data->condition))
                   == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_escalator",
                                "Failed to recognise condition name"));
          else if ((event = event_from_name (create_escalator_data->event))
                   == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_escalator",
                                "Failed to recognise event name"));
          else if ((method = escalator_method_from_name
                              (create_escalator_data->method))
                   == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_escalator",
                                "Failed to recognise method name"));
          else
            {
              switch (create_escalator (create_escalator_data->name,
                                        create_escalator_data->comment,
                                        event,
                                        create_escalator_data->event_data,
                                        condition,
                                        create_escalator_data->condition_data,
                                        method,
                                        create_escalator_data->method_data,
                                        &new_escalator))
                {
                  case 0:
                    {
                      char *uuid;
                      escalator_uuid (new_escalator, &uuid);
                      SENDF_TO_CLIENT_OR_FAIL
                       (XML_OK_CREATED_ID ("create_escalator"), uuid);
                      g_log ("event escalator", G_LOG_LEVEL_MESSAGE,
                             "Escalator %s has been created", uuid);
                      free (uuid);
                      break;
                    }
                  case 1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_escalator",
                                        "Escalator exists already"));
                    g_log ("event escalator", G_LOG_LEVEL_MESSAGE,
                           "Escalator could not be created");
                    break;
                  default:
                    assert (0);
                  case -1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("create_escalator"));
                    g_log ("event escalator", G_LOG_LEVEL_MESSAGE,
                           "Escalator could not be created");
                    break;
                }
            }
          create_escalator_data_reset (create_escalator_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      case CLIENT_CREATE_ESCALATOR_COMMENT:
        assert (strcasecmp ("COMMENT", element_name) == 0);
        set_client_state (CLIENT_CREATE_ESCALATOR);
        break;
      case CLIENT_CREATE_ESCALATOR_CONDITION:
        assert (strcasecmp ("CONDITION", element_name) == 0);
        set_client_state (CLIENT_CREATE_ESCALATOR);
        break;
      case CLIENT_CREATE_ESCALATOR_EVENT:
        assert (strcasecmp ("EVENT", element_name) == 0);
        set_client_state (CLIENT_CREATE_ESCALATOR);
        break;
      case CLIENT_CREATE_ESCALATOR_METHOD:
        assert (strcasecmp ("METHOD", element_name) == 0);
        set_client_state (CLIENT_CREATE_ESCALATOR);
        break;
      case CLIENT_CREATE_ESCALATOR_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_CREATE_ESCALATOR);
        break;

      case CLIENT_CREATE_ESCALATOR_CONDITION_DATA:
        {
          gchar *string;

          assert (strcasecmp ("DATA", element_name) == 0);
          assert (create_escalator_data->condition_data);
          assert (create_escalator_data->part_data);
          assert (create_escalator_data->part_name);

          string = g_strconcat (create_escalator_data->part_name,
                                "0",
                                create_escalator_data->part_data,
                                NULL);
          string[strlen (create_escalator_data->part_name)] = '\0';
          array_add (create_escalator_data->condition_data, string);

          openvas_free_string_var (&create_escalator_data->part_data);
          openvas_free_string_var (&create_escalator_data->part_name);
          openvas_append_string (&create_escalator_data->part_data, "");
          openvas_append_string (&create_escalator_data->part_name, "");
          set_client_state (CLIENT_CREATE_ESCALATOR_CONDITION);
          break;
        }
      case CLIENT_CREATE_ESCALATOR_CONDITION_DATA_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_CREATE_ESCALATOR_CONDITION_DATA);
        break;

      case CLIENT_CREATE_ESCALATOR_EVENT_DATA:
        {
          gchar *string;

          assert (strcasecmp ("DATA", element_name) == 0);
          assert (create_escalator_data->event_data);
          assert (create_escalator_data->part_data);
          assert (create_escalator_data->part_name);

          string = g_strconcat (create_escalator_data->part_name,
                                "0",
                                create_escalator_data->part_data,
                                NULL);
          string[strlen (create_escalator_data->part_name)] = '\0';
          array_add (create_escalator_data->event_data, string);

          openvas_free_string_var (&create_escalator_data->part_data);
          openvas_free_string_var (&create_escalator_data->part_name);
          openvas_append_string (&create_escalator_data->part_data, "");
          openvas_append_string (&create_escalator_data->part_name, "");
          set_client_state (CLIENT_CREATE_ESCALATOR_EVENT);
          break;
        }
      case CLIENT_CREATE_ESCALATOR_EVENT_DATA_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_CREATE_ESCALATOR_EVENT_DATA);
        break;

      case CLIENT_CREATE_ESCALATOR_METHOD_DATA:
        {
          gchar *string;

          assert (strcasecmp ("DATA", element_name) == 0);
          assert (create_escalator_data->method_data);
          assert (create_escalator_data->part_data);
          assert (create_escalator_data->part_name);

          string = g_strconcat (create_escalator_data->part_name,
                                "0",
                                create_escalator_data->part_data,
                                NULL);
          string[strlen (create_escalator_data->part_name)] = '\0';
          array_add (create_escalator_data->method_data, string);

          openvas_free_string_var (&create_escalator_data->part_data);
          openvas_free_string_var (&create_escalator_data->part_name);
          openvas_append_string (&create_escalator_data->part_data, "");
          openvas_append_string (&create_escalator_data->part_name, "");
          set_client_state (CLIENT_CREATE_ESCALATOR_METHOD);
          break;
        }
      case CLIENT_CREATE_ESCALATOR_METHOD_DATA_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_CREATE_ESCALATOR_METHOD_DATA);
        break;

      case CLIENT_CREATE_LSC_CREDENTIAL:
        {
          lsc_credential_t new_lsc_credential;

          assert (strcasecmp ("CREATE_LSC_CREDENTIAL", element_name) == 0);
          assert (create_lsc_credential_data->name != NULL);
          assert (create_lsc_credential_data->login != NULL);

          if (strlen (create_lsc_credential_data->name) == 0)
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_lsc_credential",
                                  "CREATE_LSC_CREDENTIAL name must be at"
                                  " least one character long"));
            }
          else if (strlen (create_lsc_credential_data->login) == 0)
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_lsc_credential",
                                  "CREATE_LSC_CREDENTIAL login must be at"
                                  " least one character long"));
            }
          else switch (create_lsc_credential
                        (create_lsc_credential_data->name,
                         create_lsc_credential_data->comment,
                         create_lsc_credential_data->login,
                         create_lsc_credential_data->password,
                         &new_lsc_credential))
            {
              case 0:
                {
                  char *uuid = lsc_credential_uuid (new_lsc_credential);
                  SENDF_TO_CLIENT_OR_FAIL
                   (XML_OK_CREATED_ID ("create_lsc_credential"), uuid);
                  free (uuid);
                  break;
                }
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_lsc_credential",
                                    "LSC Credential exists already"));
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_lsc_credential",
                                    "Login may only contain alphanumeric"
                                    " characters if autogenerating"
                                    " credential"));
                break;
              default:
                assert (0);
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_lsc_credential"));
                break;
            }
          create_lsc_credential_data_reset (create_lsc_credential_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      case CLIENT_CREATE_LSC_CREDENTIAL_COMMENT:
        assert (strcasecmp ("COMMENT", element_name) == 0);
        set_client_state (CLIENT_CREATE_LSC_CREDENTIAL);
        break;
      case CLIENT_CREATE_LSC_CREDENTIAL_LOGIN:
        assert (strcasecmp ("LOGIN", element_name) == 0);
        set_client_state (CLIENT_CREATE_LSC_CREDENTIAL);
        break;
      case CLIENT_CREATE_LSC_CREDENTIAL_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_CREATE_LSC_CREDENTIAL);
        break;
      case CLIENT_CREATE_LSC_CREDENTIAL_PASSWORD:
        assert (strcasecmp ("PASSWORD", element_name) == 0);
        set_client_state (CLIENT_CREATE_LSC_CREDENTIAL);
        break;

      case CLIENT_CREATE_NOTE:
        {
          task_t task = 0;
          result_t result = 0;
          note_t new_note;

          assert (strcasecmp ("CREATE_NOTE", element_name) == 0);

          if (create_note_data->nvt_oid == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_note",
                                "CREATE_NOTE requires an NVT entity"));
          else if (create_note_data->text == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_note",
                                "CREATE_NOTE requires a TEXT entity"));
          else if (create_note_data->task_id
                   && find_task (create_note_data->task_id, &task))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_note"));
          else if (create_note_data->task_id && task == 0)
            {
              if (send_find_error_to_client ("create_note",
                                             "task",
                                             create_note_data->task_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (create_note_data->result_id
                   && find_result (create_note_data->result_id, &result))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_note"));
          else if (create_note_data->result_id && result == 0)
            {
              if (send_find_error_to_client ("create_note",
                                             "result",
                                             create_note_data->result_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else switch (create_note (create_note_data->nvt_oid,
                                    create_note_data->text,
                                    create_note_data->hosts,
                                    create_note_data->port,
                                    create_note_data->threat,
                                    task,
                                    result,
                                    &new_note))
            {
              case 0:
                {
                  char *uuid;
                  note_uuid (new_note, &uuid);
                  SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_note"),
                                           uuid);
                  free (uuid);
                  break;
                }
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_note"));
                break;
              default:
                assert (0);
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_note"));
                break;
            }
          create_note_data_reset (create_note_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      case CLIENT_CREATE_NOTE_HOSTS:
        assert (strcasecmp ("HOSTS", element_name) == 0);
        set_client_state (CLIENT_CREATE_NOTE);
        break;
      case CLIENT_CREATE_NOTE_NVT:
        assert (strcasecmp ("NVT", element_name) == 0);
        set_client_state (CLIENT_CREATE_NOTE);
        break;
      case CLIENT_CREATE_NOTE_PORT:
        assert (strcasecmp ("PORT", element_name) == 0);
        set_client_state (CLIENT_CREATE_NOTE);
        break;
      case CLIENT_CREATE_NOTE_RESULT:
        assert (strcasecmp ("RESULT", element_name) == 0);
        set_client_state (CLIENT_CREATE_NOTE);
        break;
      case CLIENT_CREATE_NOTE_TASK:
        assert (strcasecmp ("TASK", element_name) == 0);
        set_client_state (CLIENT_CREATE_NOTE);
        break;
      case CLIENT_CREATE_NOTE_TEXT:
        assert (strcasecmp ("TEXT", element_name) == 0);
        set_client_state (CLIENT_CREATE_NOTE);
        break;
      case CLIENT_CREATE_NOTE_THREAT:
        assert (strcasecmp ("THREAT", element_name) == 0);
        set_client_state (CLIENT_CREATE_NOTE);
        break;

      case CLIENT_CREATE_OVERRIDE:
        {
          task_t task = 0;
          result_t result = 0;
          override_t new_override;

          assert (strcasecmp ("CREATE_OVERRIDE", element_name) == 0);

          if (create_override_data->nvt_oid == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_override",
                                "CREATE_OVERRIDE requires an NVT entity"));
          else if (create_override_data->text == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_override",
                                "CREATE_OVERRIDE requires a TEXT entity"));
          else if (create_override_data->new_threat == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_override",
                                "CREATE_OVERRIDE requires a NEW_THREAT"
                                " entity"));
          else if (create_override_data->task_id
              && find_task (create_override_data->task_id, &task))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_override"));
          else if (create_override_data->task_id && task == 0)
            {
              if (send_find_error_to_client ("create_override",
                                             "task",
                                             create_override_data->task_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (create_override_data->result_id
                   && find_result (create_override_data->result_id, &result))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_override"));
          else if (create_override_data->result_id && result == 0)
            {
              if (send_find_error_to_client ("create_override",
                                             "result",
                                             create_override_data->result_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else switch (create_override (create_override_data->nvt_oid,
                                        create_override_data->text,
                                        create_override_data->hosts,
                                        create_override_data->port,
                                        create_override_data->threat,
                                        create_override_data->new_threat,
                                        task,
                                        result,
                                        &new_override))
            {
              case 0:
                {
                  char *uuid;
                  override_uuid (new_override, &uuid);
                  SENDF_TO_CLIENT_OR_FAIL
                   (XML_OK_CREATED_ID ("create_override"), uuid);
                  free (uuid);
                  break;
                }
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_override"));
                break;
              default:
                assert (0);
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_override"));
                break;
            }
          create_override_data_reset (create_override_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      case CLIENT_CREATE_OVERRIDE_HOSTS:
        assert (strcasecmp ("HOSTS", element_name) == 0);
        set_client_state (CLIENT_CREATE_OVERRIDE);
        break;
      case CLIENT_CREATE_OVERRIDE_NEW_THREAT:
        assert (strcasecmp ("NEW_THREAT", element_name) == 0);
        set_client_state (CLIENT_CREATE_OVERRIDE);
        break;
      case CLIENT_CREATE_OVERRIDE_NVT:
        assert (strcasecmp ("NVT", element_name) == 0);
        set_client_state (CLIENT_CREATE_OVERRIDE);
        break;
      case CLIENT_CREATE_OVERRIDE_PORT:
        assert (strcasecmp ("PORT", element_name) == 0);
        set_client_state (CLIENT_CREATE_OVERRIDE);
        break;
      case CLIENT_CREATE_OVERRIDE_RESULT:
        assert (strcasecmp ("RESULT", element_name) == 0);
        set_client_state (CLIENT_CREATE_OVERRIDE);
        break;
      case CLIENT_CREATE_OVERRIDE_TASK:
        assert (strcasecmp ("TASK", element_name) == 0);
        set_client_state (CLIENT_CREATE_OVERRIDE);
        break;
      case CLIENT_CREATE_OVERRIDE_TEXT:
        assert (strcasecmp ("TEXT", element_name) == 0);
        set_client_state (CLIENT_CREATE_OVERRIDE);
        break;
      case CLIENT_CREATE_OVERRIDE_THREAT:
        assert (strcasecmp ("THREAT", element_name) == 0);
        set_client_state (CLIENT_CREATE_OVERRIDE);
        break;

      case CLIENT_CREATE_REPORT_FORMAT:
        {
          report_format_t new_report_format;

          assert (strcasecmp ("CREATE_REPORT_FORMAT", element_name) == 0);

          /* For now the import element, GET_REPORT_FORMATS_RESPONSE, overrides
           * any other elements. */
          if (create_report_format_data->import)
            {
              array_terminate (create_report_format_data->files);
              array_terminate (create_report_format_data->params);

              if (create_report_format_data->name == NULL)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_report_format",
                                    "CREATE_REPORT_FORMAT"
                                    " GET_REPORT_FORMATS_RESPONSE requires a"
                                    " NAME element"));
              else if (strlen (create_report_format_data->name) == 0)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_report_format",
                                    "CREATE_REPORT_FORMAT"
                                    " GET_REPORT_FORMATS_RESPONSE NAME must be"
                                    " at least one character long"));
              else if (create_report_format_data->id == NULL)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_report_format",
                                    "CREATE_REPORT_FORMAT"
                                    " GET_REPORT_FORMATS_RESPONSE requires an"
                                    " ID attribute"));
              else if (strlen (create_report_format_data->id) == 0)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_report_format",
                                    "CREATE_REPORT_FORMAT"
                                    " GET_REPORT_FORMATS_RESPONSE ID must be"
                                    " at least one character long"));
              else if (!is_uuid (create_report_format_data->id))
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_report_format",
                                    "CREATE_REPORT_FORMAT"
                                    " GET_REPORT_FORMATS_RESPONSE ID must be"
                                    " a UUID"));
              else switch (create_report_format
                            (create_report_format_data->id,
                             create_report_format_data->name,
                             create_report_format_data->content_type,
                             create_report_format_data->extension,
                             create_report_format_data->summary,
                             create_report_format_data->description,
                             create_report_format_data->global
                               && strcmp (create_report_format_data->global,
                                          "0"),
                             create_report_format_data->files,
                             create_report_format_data->params,
                             create_report_format_data->signature,
                             &new_report_format))
                {
                  case 1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_report_format",
                                        "Report format exists already"));
                    g_log ("event report_format", G_LOG_LEVEL_MESSAGE,
                           "Report format could not be created");
                    break;
                  case 2:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_report_format",
                                        "Every FILE must have a name"
                                        " attribute"));
                    g_log ("event report_format", G_LOG_LEVEL_MESSAGE,
                           "Report format could not be created");
                    break;
                  default:
                    {
                      char *uuid = report_format_uuid (new_report_format);
                      SENDF_TO_CLIENT_OR_FAIL
                       (XML_OK_CREATED_ID ("create_report_format"),
                        uuid);
                      g_log ("event report_format", G_LOG_LEVEL_MESSAGE,
                             "Report format %s has been created", uuid);
                      free (uuid);
                      break;
                    }
                }
            }
          else
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_report_format",
                                "CREATE_REPORT_FORMAT requires a"
                                " GET_REPORT_FORMATS element"));

          create_report_format_data_reset (create_report_format_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      case CLIENT_CRF_GRFR:
        assert (strcasecmp ("GET_REPORT_FORMATS_RESPONSE", element_name) == 0);
        set_client_state (CLIENT_CREATE_REPORT_FORMAT);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT:
        assert (strcasecmp ("REPORT_FORMAT", element_name) == 0);
        set_client_state (CLIENT_CRF_GRFR);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_CONTENT_TYPE:
        assert (strcasecmp ("CONTENT_TYPE", element_name) == 0);
        set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_DESCRIPTION:
        assert (strcasecmp ("DESCRIPTION", element_name) == 0);
        set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_EXTENSION:
        assert (strcasecmp ("EXTENSION", element_name) == 0);
        set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_FILE:
        {
          gchar *string;

          assert (strcasecmp ("FILE", element_name) == 0);
          assert (create_report_format_data->files);
          assert (create_report_format_data->file);
          assert (create_report_format_data->file_name);

          string = g_strconcat (create_report_format_data->file_name,
                                "0",
                                create_report_format_data->file,
                                NULL);
          string[strlen (create_report_format_data->file_name)] = '\0';
          array_add (create_report_format_data->files, string);
          openvas_free_string_var (&create_report_format_data->file);
          openvas_free_string_var (&create_report_format_data->file_name);
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT);
          break;
        }
      case CLIENT_CRF_GRFR_REPORT_FORMAT_GLOBAL:
        assert (strcasecmp ("GLOBAL", element_name) == 0);
        set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM:
        {
          gchar *string;

          assert (strcasecmp ("PARAM", element_name) == 0);
          assert (create_report_format_data->params);
          assert (create_report_format_data->param_name);
          assert (create_report_format_data->param_value);

          string = g_strconcat (create_report_format_data->param_name,
                                "0",
                                create_report_format_data->param_value,
                                NULL);
          string[strlen (create_report_format_data->param_name)] = '\0';
          array_add (create_report_format_data->params, string);
          openvas_free_string_var (&create_report_format_data->param_name);
          openvas_free_string_var (&create_report_format_data->param_value);
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT);
          break;
        }
      case CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_VALUE:
        assert (strcasecmp ("VALUE", element_name) == 0);
        set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_SIGNATURE:
        assert (strcasecmp ("SIGNATURE", element_name) == 0);
        set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_SUMMARY:
        assert (strcasecmp ("SUMMARY", element_name) == 0);
        set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_TRUST:
        assert (strcasecmp ("TRUST", element_name) == 0);
        set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT);
        break;

      case CLIENT_CREATE_SCHEDULE:
        {
          time_t first_time, period, period_months, duration;
          schedule_t new_schedule;

          period_months = 0;

          assert (strcasecmp ("CREATE_SCHEDULE", element_name) == 0);

          if (create_schedule_data->name == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_schedule",
                                "CREATE_SCHEDULE requires a NAME entity"));
          else if ((first_time = time_from_strings
                                  (create_schedule_data->first_time_hour,
                                   create_schedule_data->first_time_minute,
                                   create_schedule_data->first_time_day_of_month,
                                   create_schedule_data->first_time_month,
                                   create_schedule_data->first_time_year))
                   == -1)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_schedule",
                                "Failed to create time from FIRST_TIME"
                                " elements"));
          else if ((period = interval_from_strings
                              (create_schedule_data->period,
                               create_schedule_data->period_unit,
                               &period_months))
                   == -1)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_schedule",
                                "Failed to create interval from PERIOD"));
          else if ((duration = interval_from_strings
                                (create_schedule_data->duration,
                                 create_schedule_data->duration_unit,
                                 NULL))
                   == -1)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_schedule",
                                "Failed to create interval from DURATION"));
          else if (period_months
                   && (duration > (period_months * 60 * 60 * 24 * 28)))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_schedule",
                                "Duration too long for number of months"));
          else if (period && (duration > period))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_schedule",
                                "Duration is longer than period"));
          else switch (create_schedule (create_schedule_data->name,
                                        create_schedule_data->comment,
                                        first_time,
                                        period,
                                        period_months,
                                        duration,
                                        &new_schedule))
            {
              case 0:
                {
                  char *uuid = schedule_uuid (new_schedule);
                  SENDF_TO_CLIENT_OR_FAIL
                   (XML_OK_CREATED_ID ("create_schedule"), uuid);
                  g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                         "Schedule %s has been created", uuid);
                  free (uuid);
                  break;
                }
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_schedule",
                                    "Schedule exists already"));
                g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                       "Schedule could not be created");
                break;
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_schedule"));
                g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                       "Schedule could not be created");
                break;
              default:
                assert (0);
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_schedule"));
                g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                       "Schedule could not be created");
                break;
            }
          create_schedule_data_reset (create_schedule_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      case CLIENT_CREATE_SCHEDULE_COMMENT:
        assert (strcasecmp ("COMMENT", element_name) == 0);
        set_client_state (CLIENT_CREATE_SCHEDULE);
        break;
      case CLIENT_CREATE_SCHEDULE_DURATION:
        assert (strcasecmp ("DURATION", element_name) == 0);
        set_client_state (CLIENT_CREATE_SCHEDULE);
        break;
      case CLIENT_CREATE_SCHEDULE_FIRST_TIME:
        assert (strcasecmp ("FIRST_TIME", element_name) == 0);
        set_client_state (CLIENT_CREATE_SCHEDULE);
        break;
      case CLIENT_CREATE_SCHEDULE_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_CREATE_SCHEDULE);
        break;
      case CLIENT_CREATE_SCHEDULE_PERIOD:
        assert (strcasecmp ("PERIOD", element_name) == 0);
        set_client_state (CLIENT_CREATE_SCHEDULE);
        break;

      case CLIENT_CREATE_SCHEDULE_FIRST_TIME_DAY_OF_MONTH:
        assert (strcasecmp ("DAY_OF_MONTH", element_name) == 0);
        set_client_state (CLIENT_CREATE_SCHEDULE_FIRST_TIME);
        break;
      case CLIENT_CREATE_SCHEDULE_FIRST_TIME_HOUR:
        assert (strcasecmp ("HOUR", element_name) == 0);
        set_client_state (CLIENT_CREATE_SCHEDULE_FIRST_TIME);
        break;
      case CLIENT_CREATE_SCHEDULE_FIRST_TIME_MINUTE:
        assert (strcasecmp ("MINUTE", element_name) == 0);
        set_client_state (CLIENT_CREATE_SCHEDULE_FIRST_TIME);
        break;
      case CLIENT_CREATE_SCHEDULE_FIRST_TIME_MONTH:
        assert (strcasecmp ("MONTH", element_name) == 0);
        set_client_state (CLIENT_CREATE_SCHEDULE_FIRST_TIME);
        break;
      case CLIENT_CREATE_SCHEDULE_FIRST_TIME_YEAR:
        assert (strcasecmp ("YEAR", element_name) == 0);
        set_client_state (CLIENT_CREATE_SCHEDULE_FIRST_TIME);
        break;

      case CLIENT_CREATE_SCHEDULE_DURATION_UNIT:
        assert (strcasecmp ("UNIT", element_name) == 0);
        set_client_state (CLIENT_CREATE_SCHEDULE_DURATION);
        break;

      case CLIENT_CREATE_SCHEDULE_PERIOD_UNIT:
        assert (strcasecmp ("UNIT", element_name) == 0);
        set_client_state (CLIENT_CREATE_SCHEDULE_PERIOD);
        break;

      case CLIENT_CREATE_SLAVE:
        {
          slave_t new_slave;

          assert (strcasecmp ("CREATE_SLAVE", element_name) == 0);

          if (create_slave_data->host == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_slave",
                                "CREATE_SLAVE requires a HOST"));
          else if (strlen (create_slave_data->host) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_slave",
                                "CREATE_SLAVE HOST must be at"
                                " least one character long"));
          else if (create_slave_data->login == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_slave",
                                "CREATE_SLAVE requires a LOGIN"));
          else if (strlen (create_slave_data->login) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_slave",
                                "CREATE_SLAVE LOGIN must be at"
                                " least one character long"));
          else if (create_slave_data->name == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_slave",
                                "CREATE_SLAVE requires a NAME"));
          else if (strlen (create_slave_data->name) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_slave",
                                "CREATE_SLAVE NAME must be at"
                                " least one character long"));
          else if (create_slave_data->port == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_slave",
                                "CREATE_SLAVE requires a PORT"));
          else if (strlen (create_slave_data->port) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_slave",
                                "CREATE_SLAVE PORT must be at"
                                " least one character long"));
          /* Create slave from host string. */
          else switch (create_slave
                        (create_slave_data->name,
                         create_slave_data->comment,
                         create_slave_data->host,
                         create_slave_data->port,
                         create_slave_data->login,
                         create_slave_data->password,
                         &new_slave))
            {
              case 0:
                {
                  char *uuid = slave_uuid (new_slave);
                  SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_slave"),
                                           uuid);
                  g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                         "Slave %s has been created", uuid);
                  free (uuid);
                  break;
                }
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_slave",
                                    "Slave exists already"));
                g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                       "Slave could not be created");
                break;
              default:
                assert (0);
              case -1:
                SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_slave"));
                g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                       "Slave could not be created");
                break;
            }

          create_slave_data_reset (create_slave_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      case CLIENT_CREATE_SLAVE_COMMENT:
        assert (strcasecmp ("COMMENT", element_name) == 0);
        set_client_state (CLIENT_CREATE_SLAVE);
        break;
      case CLIENT_CREATE_SLAVE_HOST:
        assert (strcasecmp ("HOST", element_name) == 0);
        set_client_state (CLIENT_CREATE_SLAVE);
        break;
      case CLIENT_CREATE_SLAVE_LOGIN:
        assert (strcasecmp ("LOGIN", element_name) == 0);
        set_client_state (CLIENT_CREATE_SLAVE);
        break;
      case CLIENT_CREATE_SLAVE_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_CREATE_SLAVE);
        break;
      case CLIENT_CREATE_SLAVE_PASSWORD:
        assert (strcasecmp ("PASSWORD", element_name) == 0);
        set_client_state (CLIENT_CREATE_SLAVE);
        break;
      case CLIENT_CREATE_SLAVE_PORT:
        assert (strcasecmp ("PORT", element_name) == 0);
        set_client_state (CLIENT_CREATE_SLAVE);
        break;

      case CLIENT_CREATE_TARGET:
        {
          lsc_credential_t lsc_credential = 0;
          target_t new_target;

          assert (strcasecmp ("CREATE_TARGET", element_name) == 0);
          assert (&create_target_data->name != NULL);
          assert (&create_target_data->target_locator
                  || &create_target_data->hosts != NULL);

          if (strlen (create_target_data->name) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_target",
                                "CREATE_TARGET name must be at"
                                " least one character long"));
          else if (strlen (create_target_data->hosts) == 0
                   && create_target_data->target_locator == NULL)
            /** @todo Legitimate to pass an empty hosts element? */
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_target",
                                "CREATE_TARGET hosts must both be at least one"
                                " character long, or TARGET_LOCATOR must"
                                " be set"));
          else if (strlen (create_target_data->hosts) != 0
                   && create_target_data->target_locator != NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_target",
                                " CREATE_TARGET requires either a"
                                " TARGET_LOCATOR or a host"));
          else if (create_target_data->lsc_credential_id
                   && find_lsc_credential
                       (create_target_data->lsc_credential_id,
                        &lsc_credential))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_target"));
          else if (create_target_data->lsc_credential_id && lsc_credential == 0)
            {
              if (send_find_error_to_client
                   ("create_target",
                    "LSC credential",
                    create_target_data->lsc_credential_id,
                    write_to_client,
                    write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          /* Create target from host string. */
          else switch (create_target
                        (create_target_data->name,
                         create_target_data->hosts,
                         create_target_data->comment,
                         lsc_credential,
                         create_target_data->target_locator,
                         create_target_data->target_locator_username,
                         create_target_data->target_locator_password,
                         &new_target))
            {
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_target",
                                    "Target exists already"));
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target could not be created");
                break;
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_target",
                                    "Import from target_locator failed"));
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target could not be created");
                break;
              default:
                {
                  char *uuid = target_uuid (new_target);
                  SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_target"),
                                           uuid);
                  g_log ("event target", G_LOG_LEVEL_MESSAGE,
                         "Target %s has been created", uuid);
                  free (uuid);
                  break;
                }
            }

          create_target_data_reset (create_target_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      case CLIENT_CREATE_TARGET_COMMENT:
        assert (strcasecmp ("COMMENT", element_name) == 0);
        set_client_state (CLIENT_CREATE_TARGET);
        break;
      case CLIENT_CREATE_TARGET_HOSTS:
        assert (strcasecmp ("HOSTS", element_name) == 0);
        set_client_state (CLIENT_CREATE_TARGET);
        break;
      case CLIENT_CREATE_TARGET_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_CREATE_TARGET);
        break;
      case CLIENT_CREATE_TARGET_LSC_CREDENTIAL:
        assert (strcasecmp ("LSC_CREDENTIAL", element_name) == 0);
        set_client_state (CLIENT_CREATE_TARGET);
        break;
      case CLIENT_CREATE_TARGET_TARGET_LOCATOR_PASSWORD:
        assert (strcasecmp ("PASSWORD", element_name) == 0);
        set_client_state (CLIENT_CREATE_TARGET_TARGET_LOCATOR);
        break;
      case CLIENT_CREATE_TARGET_TARGET_LOCATOR:
        assert (strcasecmp ("TARGET_LOCATOR", element_name) == 0);
        set_client_state (CLIENT_CREATE_TARGET);
        break;
      case CLIENT_CREATE_TARGET_TARGET_LOCATOR_USERNAME:
        assert (strcasecmp ("USERNAME", element_name) == 0);
        set_client_state (CLIENT_CREATE_TARGET_TARGET_LOCATOR);
        break;

      case CLIENT_CREATE_TASK:
        {
          config_t config = 0;
          target_t target = 0;
          slave_t slave = 0;
          char *tsk_uuid, *name, *description;

          assert (strcasecmp ("CREATE_TASK", element_name) == 0);
          assert (create_task_data->task != (task_t) 0);

          /* The task already exists in the database at this point,
           * including the RC file (in the description column), so on
           * failure be sure to call request_delete_task to remove the
           * task. */
          /** @todo Any fail cases of the CLIENT_CREATE_TASK_* states must do
           *        so too. */

          /* Get the task ID. */

          if (task_uuid (create_task_data->task, &tsk_uuid))
            {
              request_delete_task (&create_task_data->task);
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_task"));
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }

          /* Check for the right combination of rcfile, target and config. */

          description = task_description (create_task_data->task);
          if ((description
               && (create_task_data->config_id || create_task_data->target_id))
              || (description == NULL
                  && (create_task_data->config_id == NULL
                      || create_task_data->target_id == NULL)))
            {
              request_delete_task (&create_task_data->task);
              free (tsk_uuid);
              free (description);
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_task",
                                  "CREATE_TASK requires either an rcfile"
                                  " or both a config and a target"));
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }

          assert (description
                  || (create_task_data->config_id
                      && create_task_data->target_id));

          /* Set any escalator. */

          if (create_task_data->escalator_id)
            {
              escalator_t escalator;
              if (find_escalator (create_task_data->escalator_id, &escalator))
                {
                  request_delete_task (&create_task_data->task);
                  free (tsk_uuid);
                  free (description);
                  SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_task"));
                  create_task_data_reset (create_task_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }
              if (escalator == 0)
                {
                  request_delete_task (&create_task_data->task);
                  free (tsk_uuid);
                  free (description);
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("create_task",
                                      "CREATE_TASK escalator must exist"));
                  create_task_data_reset (create_task_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }
              add_task_escalator (create_task_data->task, escalator);
            }

          /* Set any schedule. */

          if (create_task_data->schedule_id)
            {
              schedule_t schedule;
              if (find_schedule (create_task_data->schedule_id, &schedule))
                {
                  request_delete_task (&create_task_data->task);
                  free (tsk_uuid);
                  free (description);
                  SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_task"));
                  create_task_data_reset (create_task_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }
              if (schedule == 0)
                {
                  request_delete_task (&create_task_data->task);
                  free (tsk_uuid);
                  free (description);
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("create_task",
                                      "CREATE_TASK schedule must exist"));
                  create_task_data_reset (create_task_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }
              set_task_schedule (create_task_data->task, schedule);
            }

          /* Check for name. */

          name = task_name (create_task_data->task);
          if (name == NULL)
            {
              request_delete_task (&create_task_data->task);
              free (tsk_uuid);
              free (description);
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_task",
                                  "CREATE_TASK requires a name attribute"));
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }

          /* If there's an rc file, setup the target and config, otherwise
           * check that the target and config exist. */

          if (description)
            {
              int ret;
              char *hosts;
              gchar *target_name, *config_name;

              /* Create the config. */

              config_name = g_strdup_printf ("Imported config for task %s",
                                             tsk_uuid);
              ret = create_config_rc (config_name, NULL, (char*) description,
                                      &config);
              set_task_config (create_task_data->task, config);
              g_free (config_name);
              if (ret)
                {
                  request_delete_task (&create_task_data->task);
                  free (description);
                  SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_task"));
                  create_task_data_reset (create_task_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }

              /* Create the target. */

              hosts = rc_preference (description, "targets");
              if (hosts == NULL)
                {
                  request_delete_task (&create_task_data->task);
                  free (description);
                  free (tsk_uuid);
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX
                     ("create_task",
                      "CREATE_TASK rcfile must have targets"));
                  create_task_data_reset (create_task_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }
              free (description);

              target_name = g_strdup_printf ("Imported target for task %s",
                                             tsk_uuid);
              if (create_target (target_name, hosts, NULL, 0, NULL, NULL,
                                 NULL, &target))
                {
                  request_delete_task (&create_task_data->task);
                  g_free (target_name);
                  free (tsk_uuid);
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("create_task"));
                  create_task_data_reset (create_task_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }
              set_task_target (create_task_data->task, target);
              g_free (target_name);
            }
          else if (find_config (create_task_data->config_id, &config))
            {
              request_delete_task (&create_task_data->task);
              free (tsk_uuid);
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_task"));
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }
          else if (config == 0)
            {
              request_delete_task (&create_task_data->task);
              free (tsk_uuid);
              if (send_find_error_to_client ("create_task",
                                             "config",
                                             create_task_data->config_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }
          else if (find_target (create_task_data->target_id, &target))
            {
              request_delete_task (&create_task_data->task);
              free (tsk_uuid);
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_task"));
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }
          else if (target == 0)
            {
              request_delete_task (&create_task_data->task);
              free (tsk_uuid);
              if (send_find_error_to_client ("create_task",
                                             "target",
                                             create_task_data->target_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  /* Out of space. */
                  error_send_to_client (error);
                  return;
                }
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }
          else if (create_task_data->slave_id
                   && find_slave (create_task_data->slave_id, &slave))
            {
              request_delete_task (&create_task_data->task);
              free (tsk_uuid);
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_task"));
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }
          else if (create_task_data->slave_id && slave == 0)
            {
              request_delete_task (&create_task_data->task);
              free (tsk_uuid);
              if (send_find_error_to_client ("create_task",
                                             "target",
                                             create_task_data->slave_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  /* Out of space. */
                  error_send_to_client (error);
                  return;
                }
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }
          else
            {
              set_task_config (create_task_data->task, config);
              set_task_slave (create_task_data->task, slave);
              set_task_target (create_task_data->task, target);

              /* Generate the rcfile in the task. */

              if (make_task_rcfile (create_task_data->task))
                {
                  request_delete_task (&create_task_data->task);
                  free (tsk_uuid);
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("create_task",
                                      "Failed to generate task rcfile"));
                  create_task_data_reset (create_task_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }
            }

          /* Send success response. */

          SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_task"),
                                   tsk_uuid);
          g_log ("event task", G_LOG_LEVEL_MESSAGE,
                 "Task %s has been created", tsk_uuid);
          free (tsk_uuid);
          create_task_data_reset (create_task_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      case CLIENT_CREATE_TASK_COMMENT:
        assert (strcasecmp ("COMMENT", element_name) == 0);
        set_client_state (CLIENT_CREATE_TASK);
        break;
      case CLIENT_CREATE_TASK_CONFIG:
        assert (strcasecmp ("CONFIG", element_name) == 0);
        set_client_state (CLIENT_CREATE_TASK);
        break;
      case CLIENT_CREATE_TASK_ESCALATOR:
        assert (strcasecmp ("ESCALATOR", element_name) == 0);
        set_client_state (CLIENT_CREATE_TASK);
        break;
      case CLIENT_CREATE_TASK_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_CREATE_TASK);
        break;
      case CLIENT_CREATE_TASK_RCFILE:
        assert (strcasecmp ("RCFILE", element_name) == 0);
        if (create_task_data->task)
          {
            gsize out_len;
            guchar* out;
            char* description = task_description (create_task_data->task);
            if (description)
              {
                out = g_base64_decode (description, &out_len);
                /* g_base64_decode can return NULL (Glib 2.12.4-2), at least
                 * when description is zero length. */
                if (out == NULL)
                  {
                    out = (guchar*) g_strdup ("");
                    out_len = 0;
                  }
              }
            else
              {
                out = (guchar*) g_strdup ("");
                out_len = 0;
              }
            free (description);
            set_task_description (create_task_data->task, (char*) out, out_len);
            set_client_state (CLIENT_CREATE_TASK);
          }
        break;
      case CLIENT_CREATE_TASK_TARGET:
        assert (strcasecmp ("TARGET", element_name) == 0);
        set_client_state (CLIENT_CREATE_TASK);
        break;
      case CLIENT_CREATE_TASK_SCHEDULE:
        assert (strcasecmp ("SCHEDULE", element_name) == 0);
        set_client_state (CLIENT_CREATE_TASK);
        break;
      case CLIENT_CREATE_TASK_SLAVE:
        assert (strcasecmp ("SLAVE", element_name) == 0);
        set_client_state (CLIENT_CREATE_TASK);
        break;

      case CLIENT_MODIFY_NOTE:
        {
          task_t task = 0;
          result_t result = 0;
          note_t note = 0;

          assert (strcasecmp ("MODIFY_NOTE", element_name) == 0);

          if (modify_note_data->note_id == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_note",
                                "MODIFY_NOTE requires a note_id attribute"));
          else if (modify_note_data->text == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_note",
                                "MODIFY_NOTE requires a TEXT entity"));
          else if (find_note (modify_note_data->note_id, &note))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_note"));
          else if (note == 0)
            {
              if (send_find_error_to_client ("modify_note",
                                             "note",
                                             modify_note_data->note_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (modify_note_data->task_id
                   && find_task (modify_note_data->task_id, &task))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_note"));
          else if (modify_note_data->task_id && task == 0)
            {
              if (send_find_error_to_client ("modify_note",
                                             "task",
                                             modify_note_data->task_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (modify_note_data->result_id
                   && find_result (modify_note_data->result_id, &result))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_note"));
          else if (modify_note_data->result_id && result == 0)
            {
              if (send_find_error_to_client ("modify_note",
                                             "result",
                                             modify_note_data->result_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else switch (modify_note (note,
                                    modify_note_data->text,
                                    modify_note_data->hosts,
                                    modify_note_data->port,
                                    modify_note_data->threat,
                                    task,
                                    result))
            {
              case 0:
                SENDF_TO_CLIENT_OR_FAIL (XML_OK ("modify_note"));
                break;
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("modify_note"));
                break;
              default:
                assert (0);
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("modify_note"));
                break;
            }
          modify_note_data_reset (modify_note_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      case CLIENT_MODIFY_NOTE_HOSTS:
        assert (strcasecmp ("HOSTS", element_name) == 0);
        set_client_state (CLIENT_MODIFY_NOTE);
        break;
      case CLIENT_MODIFY_NOTE_PORT:
        assert (strcasecmp ("PORT", element_name) == 0);
        set_client_state (CLIENT_MODIFY_NOTE);
        break;
      case CLIENT_MODIFY_NOTE_RESULT:
        assert (strcasecmp ("RESULT", element_name) == 0);
        set_client_state (CLIENT_MODIFY_NOTE);
        break;
      case CLIENT_MODIFY_NOTE_TASK:
        assert (strcasecmp ("TASK", element_name) == 0);
        set_client_state (CLIENT_MODIFY_NOTE);
        break;
      case CLIENT_MODIFY_NOTE_TEXT:
        assert (strcasecmp ("TEXT", element_name) == 0);
        set_client_state (CLIENT_MODIFY_NOTE);
        break;
      case CLIENT_MODIFY_NOTE_THREAT:
        assert (strcasecmp ("THREAT", element_name) == 0);
        set_client_state (CLIENT_MODIFY_NOTE);
        break;

      case CLIENT_MODIFY_OVERRIDE:
        {
          task_t task = 0;
          result_t result = 0;
          override_t override = 0;

          assert (strcasecmp ("MODIFY_OVERRIDE", element_name) == 0);

          if (modify_override_data->override_id == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_override",
                                "MODIFY_OVERRIDE requires a override_id attribute"));
          else if (modify_override_data->text == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_override",
                                "MODIFY_OVERRIDE requires a TEXT entity"));
          else if (find_override (modify_override_data->override_id, &override))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_override"));
          else if (override == 0)
            {
              if (send_find_error_to_client ("modify_override",
                                             "override",
                                             modify_override_data->override_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (modify_override_data->task_id
                   && find_task (modify_override_data->task_id, &task))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_override"));
          else if (modify_override_data->task_id && task == 0)
            {
              if (send_find_error_to_client ("modify_override",
                                             "task",
                                             modify_override_data->task_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (modify_override_data->result_id
                   && find_result (modify_override_data->result_id, &result))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_override"));
          else if (modify_override_data->result_id && result == 0)
            {
              if (send_find_error_to_client ("modify_override",
                                             "result",
                                             modify_override_data->result_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else switch (modify_override (override,
                                        modify_override_data->text,
                                        modify_override_data->hosts,
                                        modify_override_data->port,
                                        modify_override_data->threat,
                                        modify_override_data->new_threat,
                                        task,
                                        result))
            {
              case 0:
                SENDF_TO_CLIENT_OR_FAIL (XML_OK ("modify_override"));
                break;
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("modify_override"));
                break;
              default:
                assert (0);
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("modify_override"));
                break;
            }
          modify_override_data_reset (modify_override_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      case CLIENT_MODIFY_OVERRIDE_HOSTS:
        assert (strcasecmp ("HOSTS", element_name) == 0);
        set_client_state (CLIENT_MODIFY_OVERRIDE);
        break;
      case CLIENT_MODIFY_OVERRIDE_NEW_THREAT:
        assert (strcasecmp ("NEW_THREAT", element_name) == 0);
        set_client_state (CLIENT_MODIFY_OVERRIDE);
        break;
      case CLIENT_MODIFY_OVERRIDE_PORT:
        assert (strcasecmp ("PORT", element_name) == 0);
        set_client_state (CLIENT_MODIFY_OVERRIDE);
        break;
      case CLIENT_MODIFY_OVERRIDE_RESULT:
        assert (strcasecmp ("RESULT", element_name) == 0);
        set_client_state (CLIENT_MODIFY_OVERRIDE);
        break;
      case CLIENT_MODIFY_OVERRIDE_TASK:
        assert (strcasecmp ("TASK", element_name) == 0);
        set_client_state (CLIENT_MODIFY_OVERRIDE);
        break;
      case CLIENT_MODIFY_OVERRIDE_TEXT:
        assert (strcasecmp ("TEXT", element_name) == 0);
        set_client_state (CLIENT_MODIFY_OVERRIDE);
        break;
      case CLIENT_MODIFY_OVERRIDE_THREAT:
        assert (strcasecmp ("THREAT", element_name) == 0);
        set_client_state (CLIENT_MODIFY_OVERRIDE);
        break;

      case CLIENT_TEST_ESCALATOR:
        if (test_escalator_data->escalator_id)
          {
            escalator_t escalator;
            task_t task;

            if (find_escalator (test_escalator_data->escalator_id, &escalator))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("test_escalator"));
            else if (escalator == 0)
              {
                if (send_find_error_to_client
                     ("test_escalator",
                      "escalator",
                      test_escalator_data->escalator_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else if (find_task (MANAGE_EXAMPLE_TASK_UUID, &task))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("test_escalator"));
            else if (task == 0)
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("test_escalator"));
            else switch (escalate (escalator,
                                   task,
                                   EVENT_TASK_RUN_STATUS_CHANGED,
                                   (void*) TASK_STATUS_DONE))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("test_escalator"));
                  break;
                case -1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("test_escalator"));
                  break;
                default: /* Programming error. */
                  assert (0);
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("test_escalator"));
                  break;
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("test_escalator",
                              "TEST_ESCALATOR requires an escalator_id"
                              " attribute"));
        test_escalator_data_reset (test_escalator_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_PAUSE_TASK:
        if (pause_task_data->task_id)
          {
            task_t task;
            if (find_task (pause_task_data->task_id, &task))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("pause_task"));
            else if (task == 0)
              {
                if (send_find_error_to_client ("pause_task",
                                               "task",
                                               pause_task_data->task_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (pause_task (task))
              {
                case 0:   /* Paused. */
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("pause_task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Task %s has been paused",
                         pause_task_data->task_id);
                  break;
                case 1:   /* Pause requested. */
                  SEND_TO_CLIENT_OR_FAIL (XML_OK_REQUESTED ("pause_task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Task %s has been requested to pause",
                         pause_task_data->task_id);
                  break;
                default:  /* Programming error. */
                  assert (0);
                case -1:
                  /* to_scanner is full. */
                  /** @todo Consider reverting parsing for retry. */
                  /** @todo process_omp_client_input must return -2. */
                  abort ();
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("pause_task"));
        pause_task_data_reset (pause_task_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_RESUME_OR_START_TASK:
        if (resume_or_start_task_data->task_id)
          {
            task_t task;
            if (find_task (resume_or_start_task_data->task_id, &task))
              SEND_TO_CLIENT_OR_FAIL
               (XML_INTERNAL_ERROR ("resume_or_start_task"));
            else if (task == 0)
              {
                if (send_find_error_to_client
                     ("resume_or_start_task",
                      "task",
                      resume_or_start_task_data->task_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else if (forked == 2)
              /* Prevent the forked child from forking again, as then both
               * forked children would be using the same server session. */
              abort (); /** @todo Respond with error or something. */
            else
              {
                char *report_id;
                switch (resume_or_start_task (task, &report_id))
                  {
                    case 0:
                      {
                        gchar *msg;
                        msg = g_strdup_printf
                               ("<resume_or_start_task_response"
                                " status=\"" STATUS_OK_REQUESTED "\""
                                " status_text=\""
                                STATUS_OK_REQUESTED_TEXT
                                "\">"
                                "<report_id>%s</report_id>"
                                "</resume_or_start_task_response>",
                                report_id);
                        free (report_id);
                        if (send_to_client (msg,
                                            write_to_client,
                                            write_to_client_data))
                          {
                            g_free (msg);
                            error_send_to_client (error);
                            return;
                          }
                        g_free (msg);
                        g_log ("event task", G_LOG_LEVEL_MESSAGE,
                               "Task %s has been requested to start",
                               resume_or_start_task_data->task_id);
                      }
                      forked = 1;
                      break;
                    case 1:
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_ERROR_SYNTAX ("resume_or_start_task",
                                          "Task is active already"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             resume_or_start_task_data->task_id);
                      break;
                    case 22:
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_ERROR_SYNTAX ("resume_or_start_task",
                                          "Task must be in Stopped state"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             resume_or_start_task_data->task_id);
                      break;
                    case 2:
                      /* Forked task process: success. */
                      current_error = 2;
                      g_set_error (error,
                                   G_MARKUP_ERROR,
                                   G_MARKUP_ERROR_INVALID_CONTENT,
                                   "Dummy error for current_error");
                      break;
                    case -10:
                      /* Forked task process: error. */
                      current_error = -10;
                      g_set_error (error,
                                   G_MARKUP_ERROR,
                                   G_MARKUP_ERROR_INVALID_CONTENT,
                                   "Dummy error for current_error");
                      break;
                    case -6:
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_ERROR_SYNTAX ("resume_or_start_task",
                                          "There is already a task running in"
                                          " this process"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             resume_or_start_task_data->task_id);
                      break;
                    case -2:
                      /* Task target lacks hosts.  This is checked when the
                       * target is created. */
                      assert (0);
                      /*@fallthrough@*/
                    case -4:
                      /* Task lacks target.  This is checked when the task is
                       * created anyway. */
                      assert (0);
                      /*@fallthrough@*/
                    case -1:
                    case -3: /* Failed to create report. */
                      SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("resume_or_start_task"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             resume_or_start_task_data->task_id);
                      break;
                    default: /* Programming error. */
                      assert (0);
                      SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("resume_or_start_task"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             resume_or_start_task_data->task_id);
                      break;
                  }
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("resume_or_start_task"));
        resume_or_start_task_data_reset (resume_or_start_task_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_RESUME_PAUSED_TASK:
        if (resume_paused_task_data->task_id)
          {
            task_t task;
            if (find_task (resume_paused_task_data->task_id, &task))
              SEND_TO_CLIENT_OR_FAIL
               (XML_INTERNAL_ERROR ("resume_paused_task"));
            else if (task == 0)
              {
                if (send_find_error_to_client
                     ("resume_paused_task",
                      "task",
                      resume_paused_task_data->task_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (resume_paused_task (task))
              {
                case 0:   /* Resumed. */
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("resume_paused_task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Task %s has been resumed",
                         resume_paused_task_data->task_id);
                  break;
                case 1:   /* Resume requested. */
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_OK_REQUESTED ("resume_paused_task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Task %s has been requested to resume",
                         resume_paused_task_data->task_id);
                  break;
                default:  /* Programming error. */
                  assert (0);
                case -1:
                  /* to_scanner is full. */
                  /** @todo Consider reverting parsing for retry. */
                  /** @todo process_omp_client_input must return -2. */
                  abort ();
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("resume_paused_task"));
        resume_paused_task_data_reset (resume_paused_task_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_RESUME_STOPPED_TASK:
        if (resume_stopped_task_data->task_id)
          {
            task_t task;
            if (find_task (resume_stopped_task_data->task_id, &task))
              SEND_TO_CLIENT_OR_FAIL
               (XML_INTERNAL_ERROR ("resume_stopped_task"));
            else if (task == 0)
              {
                if (send_find_error_to_client ("resume_stopped_task",
                                               "task",
                                               resume_stopped_task_data->task_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else if (forked == 2)
              /* Prevent the forked child from forking again, as then both
               * forked children would be using the same server session. */
              abort (); /** @todo Respond with error or something. */
            else
              {
                char *report_id;
                switch (resume_stopped_task (task, &report_id))
                  {
                    case 0:
                      {
                        gchar *msg;
                        msg = g_strdup_printf
                               ("<resume_stopped_task_response"
                                " status=\"" STATUS_OK_REQUESTED "\""
                                " status_text=\""
                                STATUS_OK_REQUESTED_TEXT
                                "\">"
                                "<report_id>%s</report_id>"
                                "</resume_stopped_task_response>",
                                report_id);
                        free (report_id);
                        if (send_to_client (msg,
                                            write_to_client,
                                            write_to_client_data))
                          {
                            g_free (msg);
                            error_send_to_client (error);
                            return;
                          }
                        g_free (msg);
                      }
                      forked = 1;
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has been resumed",
                             resume_stopped_task_data->task_id);
                      break;
                    case 1:
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_ERROR_SYNTAX ("resume_stopped_task",
                                          "Task is active already"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to resume",
                             resume_stopped_task_data->task_id);
                      break;
                    case 22:
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_ERROR_SYNTAX ("resume_stopped_task",
                                          "Task must be in Stopped state"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to resume",
                             resume_stopped_task_data->task_id);
                      break;
                    case 2:
                      /* Forked task process: success. */
                      current_error = 2;
                      g_set_error (error,
                                   G_MARKUP_ERROR,
                                   G_MARKUP_ERROR_INVALID_CONTENT,
                                   "Dummy error for current_error");
                      break;
                    case -10:
                      /* Forked task process: error. */
                      current_error = -10;
                      g_set_error (error,
                                   G_MARKUP_ERROR,
                                   G_MARKUP_ERROR_INVALID_CONTENT,
                                   "Dummy error for current_error");
                      break;
                    case -6:
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_ERROR_SYNTAX ("resume_stopped_task",
                                          "There is already a task running in"
                                          " this process"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to resume",
                             resume_stopped_task_data->task_id);
                      break;
                    case -2:
                      /* Task target lacks hosts.  This is checked when the
                       * target is created. */
                      assert (0);
                      /*@fallthrough@*/
                    case -4:
                      /* Task lacks target.  This is checked when the task is
                       * created anyway. */
                      assert (0);
                      /*@fallthrough@*/
                    case -1:
                    case -3: /* Failed to create report. */
                      SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("resume_stopped_task"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to resume",
                             resume_stopped_task_data->task_id);
                      break;
                    default: /* Programming error. */
                      assert (0);
                      SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("resume_stopped_task"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to resume",
                             resume_stopped_task_data->task_id);
                      break;
                  }
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("resume_stopped_task"));
        resume_stopped_task_data_reset (resume_stopped_task_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_START_TASK:
        if (start_task_data->task_id)
          {
            task_t task;
            if (find_task (start_task_data->task_id, &task))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("start_task"));
            else if (task == 0)
              {
                if (send_find_error_to_client ("start_task",
                                               "task",
                                               start_task_data->task_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else if (forked == 2)
              /* Prevent the forked child from forking again, as then both
               * forked children would be using the same server session. */
              abort (); /** @todo Respond with error or something. */
            else
              {
                char *report_id;
                switch (start_task (task, &report_id))
                  {
                    case 0:
                      {
                        gchar *msg;
                        msg = g_strdup_printf
                               ("<start_task_response"
                                " status=\"" STATUS_OK_REQUESTED "\""
                                " status_text=\""
                                STATUS_OK_REQUESTED_TEXT
                                "\">"
                                "<report_id>%s</report_id>"
                                "</start_task_response>",
                                report_id);
                        free (report_id);
                        if (send_to_client (msg,
                                            write_to_client,
                                            write_to_client_data))
                          {
                            g_free (msg);
                            error_send_to_client (error);
                            return;
                          }
                        g_free (msg);
                        g_log ("event task", G_LOG_LEVEL_MESSAGE,
                               "Task %s has been requested to start",
                               start_task_data->task_id);
                      }
                      forked = 1;
                      break;
                    case 1:
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_ERROR_SYNTAX ("start_task",
                                          "Task is active already"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             start_task_data->task_id);
                      break;
                    case 2:
                      /* Forked task process: success. */
                      current_error = 2;
                      g_set_error (error,
                                   G_MARKUP_ERROR,
                                   G_MARKUP_ERROR_INVALID_CONTENT,
                                   "Dummy error for current_error");
                      break;
                    case -10:
                      /* Forked task process: error. */
                      current_error = -10;
                      g_set_error (error,
                                   G_MARKUP_ERROR,
                                   G_MARKUP_ERROR_INVALID_CONTENT,
                                   "Dummy error for current_error");
                      break;
                    case -6:
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_ERROR_SYNTAX ("start_task",
                                          "There is already a task running in"
                                          " this process"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             start_task_data->task_id);
                      break;
                    case -2:
                      /* Task target lacks hosts.  This is checked when the
                       * target is created. */
                      assert (0);
                      /*@fallthrough@*/
                    case -4:
                      /* Task lacks target.  This is checked when the task is
                       * created anyway. */
                      assert (0);
                      /*@fallthrough@*/
                    case -3: /* Failed to create report. */
                      SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("start_task"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             start_task_data->task_id);
                      break;
                    default: /* Programming error. */
                      assert (0);
                      SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("start_task"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             start_task_data->task_id);
                      break;
                  }
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL (XML_ERROR_SYNTAX ("start_task",
                                                    "START_TASK task_id"
                                                    " attribute must be set"));
        start_task_data_reset (start_task_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_STOP_TASK:
        if (stop_task_data->task_id)
          {
            task_t task;

            if (find_task (stop_task_data->task_id, &task))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("stop_task"));
            else if (task == 0)
              {
                if (send_find_error_to_client ("stop_task",
                                               "task",
                                               stop_task_data->task_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (stop_task (task))
              {
                case 0:   /* Stopped. */
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("stop_task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Task %s has been stopped",
                         stop_task_data->task_id);
                  break;
                case 1:   /* Stop requested. */
                  SEND_TO_CLIENT_OR_FAIL (XML_OK_REQUESTED ("stop_task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Task %s has been requested to stop",
                         stop_task_data->task_id);
                  break;
                default:  /* Programming error. */
                  assert (0);
                case -1:
                  /* to_scanner is full. */
                  /** @todo Consider reverting parsing for retry. */
                  /** @todo process_omp_client_input must return -2. */
                  abort ();
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("stop_task",
                              "STOP_TASK requires a task_id attribute"));
        stop_task_data_reset (stop_task_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_GET_AGENTS:
        {
          iterator_t agents;
          int format;
          agent_t agent = 0;

          assert (strcasecmp ("GET_AGENTS", element_name) == 0);

          if (get_agents_data->format)
            {
              if (strlen (get_agents_data->format))
                {
                  if (strcasecmp (get_agents_data->format, "installer") == 0)
                    format = 1;
                  else if (strcasecmp (get_agents_data->format,
                                       "howto_install")
                           == 0)
                    format = 2;
                  else if (strcasecmp (get_agents_data->format, "howto_use")
                           == 0)
                    format = 3;
                  else
                    format = -1;
                }
              else
                format = 0;
            }
          else
            format = 0;
          if (format == -1)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_agents",
                                "GET_AGENTS format attribute should"
                                " be \"installer\", \"howto_install\" or \"howto_use\"."));
          else if (get_agents_data->agent_id
                   && find_agent (get_agents_data->agent_id, &agent))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_agents"));
          else if (get_agents_data->agent_id && agent == 0)
            {
              if (send_find_error_to_client ("get_agents",
                                             "agent",
                                             get_agents_data->agent_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              SEND_TO_CLIENT_OR_FAIL ("<get_agents_response"
                                      " status=\"" STATUS_OK "\""
                                      " status_text=\"" STATUS_OK_TEXT "\">");
              init_agent_iterator (&agents,
                                   agent,
                                   get_agents_data->sort_order,
                                   get_agents_data->sort_field);
              while (next (&agents))
                {
                  switch (format)
                    {
                      case 1: /* installer */
                        SENDF_TO_CLIENT_OR_FAIL
                         ("<agent id=\"%s\">"
                          "<name>%s</name>"
                          "<comment>%s</comment>"
                          "<package format=\"installer\">"
                          "<filename>%s</filename>"
                          "%s"
                          "</package>"
                          "<in_use>0</in_use>"
                          "</agent>",
                          agent_iterator_uuid (&agents),
                          agent_iterator_name (&agents),
                          agent_iterator_comment (&agents),
                          agent_iterator_installer_filename (&agents),
                          agent_iterator_installer_64 (&agents));
                        break;
                      case 2: /* howto_install */
                        SENDF_TO_CLIENT_OR_FAIL
                         ("<agent id=\"%s\">"
                          "<name>%s</name>"
                          "<comment>%s</comment>"
                          "<package format=\"howto_install\">%s</package>"
                          "<in_use>0</in_use>"
                          "</agent>",
                          agent_iterator_uuid (&agents),
                          agent_iterator_name (&agents),
                          agent_iterator_comment (&agents),
                          agent_iterator_howto_install (&agents));
                        break;
                      case 3: /* howto_use */
                        SENDF_TO_CLIENT_OR_FAIL
                         ("<agent id=\"%s\">"
                          "<name>%s</name>"
                          "<comment>%s</comment>"
                          "<package format=\"howto_use\">%s</package>"
                          "<in_use>0</in_use>"
                          "</agent>",
                          agent_iterator_uuid (&agents),
                          agent_iterator_name (&agents),
                          agent_iterator_comment (&agents),
                          agent_iterator_howto_use (&agents));
                        break;
                      default:
                        {
                          time_t trust_time;

                          trust_time = agent_iterator_trust_time (&agents);

                          SENDF_TO_CLIENT_OR_FAIL
                           ("<agent id=\"%s\">"
                            "<name>%s</name>"
                            "<comment>%s</comment>"
                            "<in_use>0</in_use>"
                            "<installer>"
                            "<trust>%s<time>%s</time></trust>"
                            "</installer>"
                            "</agent>",
                            agent_iterator_uuid (&agents),
                            agent_iterator_name (&agents),
                            agent_iterator_comment (&agents),
                            agent_iterator_trust (&agents),
                            ctime_strip_newline (&trust_time));
                        }
                        break;
                    }
                }
              cleanup_iterator (&agents);
              SEND_TO_CLIENT_OR_FAIL ("</get_agents_response>");
            }
          get_agents_data_reset (get_agents_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_CONFIGS:
        {
          config_t request_config = 0;
          iterator_t configs;

          assert (strcasecmp ("GET_CONFIGS", element_name) == 0);

          if (get_configs_data->config_id
              && find_config (get_configs_data->config_id, &request_config))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_configs"));
          else if (get_configs_data->config_id && (request_config == 0))
            {
              if (send_find_error_to_client ("get_configs",
                                             "config",
                                             get_configs_data->config_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              SEND_TO_CLIENT_OR_FAIL ("<get_configs_response"
                                      " status=\"" STATUS_OK "\""
                                      " status_text=\"" STATUS_OK_TEXT "\">");
              init_config_iterator (&configs,
                                    request_config,
                                    get_configs_data->sort_order,
                                    get_configs_data->sort_field);
              while (next (&configs))
                {
                  int config_nvts_growing, config_families_growing;
                  const char *selector;
                  config_t config;
                  iterator_t tasks;

                  /** @todo This should really be an nvt_selector_t. */
                  selector = config_iterator_nvt_selector (&configs);
                  config = config_iterator_config (&configs);
                  config_nvts_growing = config_iterator_nvts_growing (&configs);
                  config_families_growing
                    = config_iterator_families_growing (&configs);

                  if (get_configs_data->export)
                    SENDF_TO_CLIENT_OR_FAIL ("<config id=\"%s\">"
                                             "<name>%s</name>"
                                             "<comment>%s</comment>",
                                             config_iterator_uuid (&configs),
                                             config_iterator_name (&configs),
                                             config_iterator_comment
                                              (&configs));
                  else
                    {
                      SENDF_TO_CLIENT_OR_FAIL ("<config id=\"%s\">"
                                               "<name>%s</name>"
                                               "<comment>%s</comment>"
                                               "<family_count>"
                                               "%i<growing>%i</growing>"
                                               "</family_count>"
                                               /* The number of NVT's selected
                                                * by the selector. */
                                               "<nvt_count>"
                                               "%i<growing>%i</growing>"
                                               "</nvt_count>"
                                               "<in_use>%i</in_use>"
                                               "<tasks>",
                                               config_iterator_uuid (&configs),
                                               config_iterator_name (&configs),
                                               config_iterator_comment
                                                (&configs),
                                               config_family_count (config),
                                               config_families_growing,
                                               config_nvt_count (config),
                                               config_nvts_growing,
                                               config_in_use (config));

                      init_config_task_iterator (&tasks,
                                                 config,
                                                 get_configs_data->sort_order);
                      while (next (&tasks))
                        SENDF_TO_CLIENT_OR_FAIL
                         ("<task id=\"%s\">"
                          "<name>%s</name>"
                          "</task>",
                          config_task_iterator_uuid (&tasks),
                          config_task_iterator_name (&tasks));
                      cleanup_iterator (&tasks);
                      SEND_TO_CLIENT_OR_FAIL ("</tasks>");

                      if (get_configs_data->families)
                        {
                          iterator_t families;
                          int max_nvt_count = 0, known_nvt_count = 0;

                          SENDF_TO_CLIENT_OR_FAIL ("<families>");
                          init_family_iterator (&families,
                                                config_families_growing,
                                                selector,
                                                get_configs_data->sort_order);
                          while (next (&families))
                            {
                              int family_growing, family_max;
                              int family_selected_count;
                              const char *family;

                              family = family_iterator_name (&families);
                              if (family)
                                {
                                  family_growing = nvt_selector_family_growing
                                                    (selector,
                                                     family,
                                                     config_families_growing);
                                  family_max = family_nvt_count (family);
                                  family_selected_count
                                    = nvt_selector_nvt_count (selector,
                                                              family,
                                                              family_growing);
                                  known_nvt_count += family_selected_count;
                                }
                              else
                                {
                                  /* The family can be NULL if an RC adds an
                                   * NVT to a config and the NVT is missing
                                   * from the NVT cache. */
                                  family_growing = 0;
                                  family_max = -1;
                                  family_selected_count = nvt_selector_nvt_count
                                                           (selector, NULL, 0);
                                }

                              SENDF_TO_CLIENT_OR_FAIL
                               ("<family>"
                                "<name>%s</name>"
                                /* The number of selected NVT's. */
                                "<nvt_count>%i</nvt_count>"
                                /* The total number of NVT's in the family. */
                                "<max_nvt_count>%i</max_nvt_count>"
                                "<growing>%i</growing>"
                                "</family>",
                                family ? family : "",
                                family_selected_count,
                                family_max,
                                family_growing);
                              if (family_max > 0)
                                max_nvt_count += family_max;
                            }
                          cleanup_iterator (&families);
                          SENDF_TO_CLIENT_OR_FAIL
                           ("</families>"
                            /* The total number of NVT's in all the
                             * families for selector selects at least one
                             * NVT. */
                            "<max_nvt_count>%i</max_nvt_count>"
                            /* Total number of selected known NVT's. */
                            "<known_nvt_count>"
                            "%i"
                            "</known_nvt_count>",
                            max_nvt_count,
                            known_nvt_count);
                        }
                    }

                  if (get_configs_data->preferences || get_configs_data->export)
                    {
                      iterator_t prefs;
                      config_t config = config_iterator_config (&configs);

                      assert (config);

                      SEND_TO_CLIENT_OR_FAIL ("<preferences>");

                      init_nvt_preference_iterator (&prefs, NULL);
                      while (next (&prefs))
                        {
                          GString *buffer = g_string_new ("");
                          buffer_config_preference_xml (buffer, &prefs, config);
                          SEND_TO_CLIENT_OR_FAIL (buffer->str);
                          g_string_free (buffer, TRUE);
                        }
                      cleanup_iterator (&prefs);

                      SEND_TO_CLIENT_OR_FAIL ("</preferences>");
                    }

                  if (get_configs_data->export)
                    {
                      iterator_t selectors;

                      SEND_TO_CLIENT_OR_FAIL ("<nvt_selectors>");

                      init_nvt_selector_iterator (&selectors,
                                                  NULL,
                                                  config,
                                                  NVT_SELECTOR_TYPE_ANY);
                      while (next (&selectors))
                        {
                          int type = nvt_selector_iterator_type (&selectors);
                          SENDF_TO_CLIENT_OR_FAIL
                           ("<nvt_selector>"
                            "<name>%s</name>"
                            "<include>%i</include>"
                            "<type>%i</type>"
                            "<family_or_nvt>%s</family_or_nvt>"
                            "</nvt_selector>",
                            nvt_selector_iterator_name (&selectors),
                            nvt_selector_iterator_include (&selectors),
                            type,
                            (type == NVT_SELECTOR_TYPE_ALL
                              ? ""
                              : nvt_selector_iterator_nvt (&selectors)));
                        }
                      cleanup_iterator (&selectors);

                      SEND_TO_CLIENT_OR_FAIL ("</nvt_selectors>");
                    }

                  SENDF_TO_CLIENT_OR_FAIL ("</config>");
                }
            }
          cleanup_iterator (&configs);
          get_configs_data_reset (get_configs_data);
          SEND_TO_CLIENT_OR_FAIL ("</get_configs_response>");
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_ESCALATORS:
        {
          escalator_t escalator = 0;

          assert (strcasecmp ("GET_ESCALATORS", element_name) == 0);

          if (get_escalators_data->escalator_id
              && find_escalator (get_escalators_data->escalator_id, &escalator))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_escalators"));
          else if (get_escalators_data->escalator_id && escalator == 0)
            {
              if (send_find_error_to_client ("get_escalators",
                                             "escalator",
                                             get_escalators_data->escalator_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              iterator_t escalators;

              SEND_TO_CLIENT_OR_FAIL ("<get_escalators_response"
                                      " status=\"" STATUS_OK "\""
                                      " status_text=\"" STATUS_OK_TEXT "\">");
              init_escalator_iterator (&escalators,
                                       escalator,
                                       (task_t) 0,
                                       (event_t) 0,
                                       get_escalators_data->sort_order,
                                       get_escalators_data->sort_field);
              while (next (&escalators))
                {
                  iterator_t data;

                  SENDF_TO_CLIENT_OR_FAIL ("<escalator id=\"%s\">"
                                           "<name>%s</name>"
                                           "<comment>%s</comment>"
                                           "<in_use>%i</in_use>",
                                           escalator_iterator_uuid (&escalators),
                                           escalator_iterator_name (&escalators),
                                           escalator_iterator_comment (&escalators),
                                           escalator_iterator_in_use (&escalators));

                  /* Condition. */

                  SENDF_TO_CLIENT_OR_FAIL ("<condition>%s",
                                           escalator_condition_name
                                            (escalator_iterator_condition
                                              (&escalators)));
                  init_escalator_data_iterator (&data,
                                                escalator_iterator_escalator
                                                 (&escalators),
                                                "condition");
                  while (next (&data))
                    SENDF_TO_CLIENT_OR_FAIL ("<data>"
                                             "<name>%s</name>"
                                             "%s"
                                             "</data>",
                                             escalator_data_iterator_name (&data),
                                             escalator_data_iterator_data (&data));
                  cleanup_iterator (&data);
                  SEND_TO_CLIENT_OR_FAIL ("</condition>");

                  /* Event. */

                  SENDF_TO_CLIENT_OR_FAIL ("<event>%s",
                                           event_name (escalator_iterator_event
                                            (&escalators)));
                  init_escalator_data_iterator (&data,
                                                escalator_iterator_escalator
                                                 (&escalators),
                                                "event");
                  while (next (&data))
                    SENDF_TO_CLIENT_OR_FAIL ("<data>"
                                             "<name>%s</name>"
                                             "%s"
                                             "</data>",
                                             escalator_data_iterator_name (&data),
                                             escalator_data_iterator_data (&data));
                  cleanup_iterator (&data);
                  SEND_TO_CLIENT_OR_FAIL ("</event>");

                  /* Method. */

                  SENDF_TO_CLIENT_OR_FAIL ("<method>%s",
                                           escalator_method_name
                                            (escalator_iterator_method
                                              (&escalators)));
                  init_escalator_data_iterator (&data,
                                                escalator_iterator_escalator
                                                 (&escalators),
                                                "method");
                  while (next (&data))
                    SENDF_TO_CLIENT_OR_FAIL ("<data>"
                                             "<name>%s</name>"
                                             "%s"
                                             "</data>",
                                             escalator_data_iterator_name (&data),
                                             escalator_data_iterator_data (&data));
                  cleanup_iterator (&data);
                  SEND_TO_CLIENT_OR_FAIL ("</method>");

                  /**
                   * @todo
                   * (OMP) For consistency, the operations should respond the
                   * same way if one, some or all elements are requested.  The
                   * level of details in the response should instead be controlled
                   * by some other mechanism, like a details flag.
                   */

                  if (escalator)
                    {
                      iterator_t tasks;

                      SEND_TO_CLIENT_OR_FAIL ("<tasks>");
                      init_escalator_task_iterator
                       (&tasks,
                        escalator,
                        get_escalators_data->sort_order);
                      while (next (&tasks))
                        SENDF_TO_CLIENT_OR_FAIL
                         ("<task id=\"%s\">"
                          "<name>%s</name>"
                          "</task>",
                          escalator_task_iterator_uuid (&tasks),
                          escalator_task_iterator_name (&tasks));
                      cleanup_iterator (&tasks);
                      SEND_TO_CLIENT_OR_FAIL ("</tasks>");
                    }

                  SEND_TO_CLIENT_OR_FAIL ("</escalator>");
                }
              cleanup_iterator (&escalators);
              SEND_TO_CLIENT_OR_FAIL ("</get_escalators_response>");
            }
          get_escalators_data_reset (get_escalators_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_LSC_CREDENTIALS:
        {
          iterator_t credentials;
          int format;
          lsc_credential_t lsc_credential = 0;
          char *data_format;

          assert (strcasecmp ("GET_LSC_CREDENTIALS", element_name) == 0);

          data_format = get_lsc_credentials_data->format;
          if (data_format)
            {
              if (strlen (data_format))
                {
                  if (strcasecmp (data_format, "key") == 0)
                    format = 1;
                  else if (strcasecmp (data_format, "rpm") == 0)
                    format = 2;
                  else if (strcasecmp (data_format, "deb") == 0)
                    format = 3;
                  else if (strcasecmp (data_format, "exe") == 0)
                    format = 4;
                  else
                    format = -1;
                }
              else
                format = 0;
            }
          else
            format = 0;

          if (format == -1)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_lsc_credentials",
                                "GET_LSC_CREDENTIALS format attribute should"
                                " be \"key\", \"rpm\", \"deb\" or \"exe\"."));
          else if (get_lsc_credentials_data->lsc_credential_id
                   && find_lsc_credential
                       (get_lsc_credentials_data->lsc_credential_id,
                        &lsc_credential))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_lsc_credentials"));
          else if (get_lsc_credentials_data->lsc_credential_id
                   && (lsc_credential == 0))
            {
              if (send_find_error_to_client
                   ("get_lsc_credentials",
                    "LSC credential",
                    get_lsc_credentials_data->lsc_credential_id,
                    write_to_client,
                    write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              SEND_TO_CLIENT_OR_FAIL ("<get_lsc_credentials_response"
                                      " status=\"" STATUS_OK "\""
                                      " status_text=\"" STATUS_OK_TEXT "\">");
              init_lsc_credential_iterator (&credentials,
                                            lsc_credential,
                                            get_lsc_credentials_data->sort_order,
                                            get_lsc_credentials_data->sort_field);
              while (next (&credentials))
                {
                  switch (format)
                    {
                      case 1: /* key */
                        SENDF_TO_CLIENT_OR_FAIL
                         ("<lsc_credential id=\"%s\">"
                          "<name>%s</name>"
                          "<login>%s</login>"
                          "<comment>%s</comment>"
                          "<in_use>%i</in_use>"
                          "<type>%s</type>"
                          "<public_key>%s</public_key>"
                          "</lsc_credential>",
                          lsc_credential_iterator_uuid (&credentials),
                          lsc_credential_iterator_name (&credentials),
                          lsc_credential_iterator_login (&credentials),
                          lsc_credential_iterator_comment (&credentials),
                          lsc_credential_iterator_in_use (&credentials),
                          lsc_credential_iterator_public_key (&credentials)
                            ? "gen" : "pass",
                          lsc_credential_iterator_public_key (&credentials)
                            ? lsc_credential_iterator_public_key (&credentials)
                            : "");
                        break;
                      case 2: /* rpm */
                        SENDF_TO_CLIENT_OR_FAIL
                         ("<lsc_credential id=\"%s\">"
                          "<name>%s</name>"
                          "<login>%s</login>"
                          "<comment>%s</comment>"
                          "<in_use>%i</in_use>"
                          "<type>%s</type>"
                          "<package format=\"rpm\">%s</package>"
                          "</lsc_credential>",
                          lsc_credential_iterator_uuid (&credentials),
                          lsc_credential_iterator_name (&credentials),
                          lsc_credential_iterator_login (&credentials),
                          lsc_credential_iterator_comment (&credentials),
                          lsc_credential_iterator_in_use (&credentials),
                          lsc_credential_iterator_public_key (&credentials)
                            ? "gen" : "pass",
                          lsc_credential_iterator_rpm (&credentials)
                            ? lsc_credential_iterator_rpm (&credentials)
                            : "");
                        break;
                      case 3: /* deb */
                        SENDF_TO_CLIENT_OR_FAIL
                         ("<lsc_credential id=\"%s\">"
                          "<name>%s</name>"
                          "<login>%s</login>"
                          "<comment>%s</comment>"
                          "<in_use>%i</in_use>"
                          "<type>%s</type>"
                          "<package format=\"deb\">%s</package>"
                          "</lsc_credential>",
                          lsc_credential_iterator_uuid (&credentials),
                          lsc_credential_iterator_name (&credentials),
                          lsc_credential_iterator_login (&credentials),
                          lsc_credential_iterator_comment (&credentials),
                          lsc_credential_iterator_in_use (&credentials),
                          lsc_credential_iterator_public_key (&credentials)
                            ? "gen" : "pass",
                          lsc_credential_iterator_deb (&credentials)
                            ? lsc_credential_iterator_deb (&credentials)
                            : "");
                        break;
                      case 4: /* exe */
                        SENDF_TO_CLIENT_OR_FAIL
                         ("<lsc_credential id=\"%s\">"
                          "<name>%s</name>"
                          "<login>%s</login>"
                          "<comment>%s</comment>"
                          "<in_use>%i</in_use>"
                          "<type>%s</type>"
                          "<package format=\"exe\">%s</package>"
                          "</lsc_credential>",
                          lsc_credential_iterator_uuid (&credentials),
                          lsc_credential_iterator_name (&credentials),
                          lsc_credential_iterator_login (&credentials),
                          lsc_credential_iterator_comment (&credentials),
                          lsc_credential_iterator_in_use (&credentials),
                          lsc_credential_iterator_public_key (&credentials)
                            ? "gen" : "pass",
                          lsc_credential_iterator_exe (&credentials)
                            ? lsc_credential_iterator_exe (&credentials)
                            : "");
                        break;
                      default:
                        {
                          iterator_t targets;

                          SENDF_TO_CLIENT_OR_FAIL
                           ("<lsc_credential id=\"%s\">"
                            "<name>%s</name>"
                            "<login>%s</login>"
                            "<comment>%s</comment>"
                            "<in_use>%i</in_use>"
                            "<type>%s</type>"
                            "<targets>",
                            lsc_credential_iterator_uuid (&credentials),
                            lsc_credential_iterator_name (&credentials),
                            lsc_credential_iterator_login (&credentials),
                            lsc_credential_iterator_comment (&credentials),
                            lsc_credential_iterator_in_use (&credentials),
                            lsc_credential_iterator_public_key (&credentials)
                              ? "gen" : "pass");

                          init_lsc_credential_target_iterator
                           (&targets,
                            lsc_credential_iterator_lsc_credential
                             (&credentials),
                            get_lsc_credentials_data->sort_order);
                          while (next (&targets))
                            SENDF_TO_CLIENT_OR_FAIL
                             ("<target id=\"%s\">"
                              "<name>%s</name>"
                              "</target>",
                              lsc_credential_target_iterator_uuid (&targets),
                              lsc_credential_target_iterator_name (&targets));
                          cleanup_iterator (&targets);

                          SEND_TO_CLIENT_OR_FAIL ("</targets>"
                                                  "</lsc_credential>");
                          break;
                        }
                    }
                }
              cleanup_iterator (&credentials);
              SEND_TO_CLIENT_OR_FAIL ("</get_lsc_credentials_response>");
            }
          get_lsc_credentials_data_reset (get_lsc_credentials_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_SLAVES:
        {
          slave_t slave = 0;

          assert (strcasecmp ("GET_SLAVES", element_name) == 0);

          if (get_slaves_data->slave_id
              && find_slave (get_slaves_data->slave_id, &slave))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_slaves"));
          else if (get_slaves_data->slave_id && slave == 0)
            {
              if (send_find_error_to_client ("get_slaves",
                                             "slave",
                                             get_slaves_data->slave_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              iterator_t slaves;

              SEND_TO_CLIENT_OR_FAIL ("<get_slaves_response"
                                      " status=\"" STATUS_OK "\""
                                      " status_text=\"" STATUS_OK_TEXT "\">");
              init_slave_iterator (&slaves,
                                   slave,
                                   get_slaves_data->sort_order,
                                   get_slaves_data->sort_field);
              while (next (&slaves))
                {
                  SENDF_TO_CLIENT_OR_FAIL ("<slave id=\"%s\">"
                                           "<name>%s</name>"
                                           "<comment>%s</comment>"
                                           "<host>%s</host>"
                                           "<port>%s</port>"
                                           "<login>%s</login>"
                                           "<in_use>%i</in_use>",
                                           slave_iterator_uuid (&slaves),
                                           slave_iterator_name (&slaves),
                                           slave_iterator_comment (&slaves),
                                           slave_iterator_host (&slaves),
                                           slave_iterator_port (&slaves),
                                           slave_iterator_login (&slaves),
                                           slave_in_use
                                            (slave_iterator_slave (&slaves)));

                  if (get_slaves_data->tasks)
                    {
                      iterator_t tasks;

                      SEND_TO_CLIENT_OR_FAIL ("<tasks>");
                      init_slave_task_iterator (&tasks,
                                                slave_iterator_slave
                                                 (&slaves),
                                                get_slaves_data->sort_order);
                      while (next (&tasks))
                        SENDF_TO_CLIENT_OR_FAIL
                         ("<task id=\"%s\">"
                          "<name>%s</name>"
                          "</task>",
                          slave_task_iterator_uuid (&tasks),
                          slave_task_iterator_name (&tasks));
                      cleanup_iterator (&tasks);
                      SEND_TO_CLIENT_OR_FAIL ("</tasks>");
                    }

                  SEND_TO_CLIENT_OR_FAIL ("</slave>");
                }
              cleanup_iterator (&slaves);
              SEND_TO_CLIENT_OR_FAIL ("</get_slaves_response>");
            }
          get_slaves_data_reset (get_slaves_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_SYSTEM_REPORTS:
        {
          assert (strcasecmp ("GET_SYSTEM_REPORTS", element_name) == 0);

          report_type_iterator_t types;

          if (init_system_report_type_iterator (&types,
                                                get_system_reports_data->name))
            SEND_TO_CLIENT_OR_FAIL
             (XML_INTERNAL_ERROR ("get_system_reports"));
          else
            {
              char *report;
              SEND_TO_CLIENT_OR_FAIL ("<get_system_reports_response"
                                      " status=\"" STATUS_OK "\""
                                      " status_text=\"" STATUS_OK_TEXT "\">");
              while (next_report_type (&types))
                if (get_system_reports_data->brief)
                  SENDF_TO_CLIENT_OR_FAIL
                   ("<system_report>"
                    "<name>%s</name>"
                    "<title>%s</title>"
                    "</system_report>",
                    report_type_iterator_name (&types),
                    report_type_iterator_title (&types));
                else if (manage_system_report
                          (report_type_iterator_name (&types),
                           get_system_reports_data->duration,
                           &report))
                  {
                    cleanup_report_type_iterator (&types);
                    internal_error_send_to_client (error);
                    return;
                  }
                else if (report)
                  {
                    SENDF_TO_CLIENT_OR_FAIL
                     ("<system_report>"
                      "<name>%s</name>"
                      "<title>%s</title>"
                      "<report format=\"png\" duration=\"%s\">"
                      "%s"
                      "</report>"
                      "</system_report>",
                      report_type_iterator_name (&types),
                      report_type_iterator_title (&types),
                      get_system_reports_data->duration
                       ? get_system_reports_data->duration
                       : "86400",
                      report);
                    free (report);
                  }
              cleanup_report_type_iterator (&types);
              SEND_TO_CLIENT_OR_FAIL ("</get_system_reports_response>");
            }

          get_system_reports_data_reset (get_system_reports_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_TARGETS:
        {
          target_t target = 0;

          assert (strcasecmp ("GET_TARGETS", element_name) == 0);

          if (get_targets_data->target_id
              && find_target (get_targets_data->target_id, &target))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_targets"));
          else if (get_targets_data->target_id && target == 0)
            {
              if (send_find_error_to_client ("get_targets",
                                             "target",
                                             get_targets_data->target_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              iterator_t targets;

              SEND_TO_CLIENT_OR_FAIL ("<get_targets_response"
                                      " status=\"" STATUS_OK "\""
                                      " status_text=\"" STATUS_OK_TEXT "\">");
              init_target_iterator (&targets,
                                    target,
                                    get_targets_data->sort_order,
                                    get_targets_data->sort_field);
              while (next (&targets))
                {
                  char *lsc_name, *lsc_uuid;
                  lsc_credential_t lsc_credential;

                  lsc_credential = target_iterator_lsc_credential (&targets);
                  lsc_name = lsc_credential_name (lsc_credential);
                  lsc_uuid = lsc_credential_uuid (lsc_credential);
                  SENDF_TO_CLIENT_OR_FAIL ("<target id=\"%s\">"
                                           "<name>%s</name>"
                                           "<hosts>%s</hosts>"
                                           "<max_hosts>%i</max_hosts>"
                                           "<comment>%s</comment>"
                                           "<in_use>%i</in_use>"
                                           "<lsc_credential id=\"%s\">"
                                           "<name>%s</name>"
                                           "</lsc_credential>",
                                           target_iterator_uuid (&targets),
                                           target_iterator_name (&targets),
                                           target_iterator_hosts (&targets),
                                           max_hosts
                                            (target_iterator_hosts (&targets)),
                                           target_iterator_comment (&targets),
                                           target_in_use
                                            (target_iterator_target (&targets)),
                                           lsc_uuid ? lsc_uuid : "",
                                           lsc_name ? lsc_name : "");

                  if (get_targets_data->tasks)
                    {
                      iterator_t tasks;

                      SEND_TO_CLIENT_OR_FAIL ("<tasks>");
                      init_target_task_iterator (&tasks,
                                                 target_iterator_target
                                                  (&targets),
                                                 get_targets_data->sort_order);
                      while (next (&tasks))
                        SENDF_TO_CLIENT_OR_FAIL ("<task id=\"%s\">"
                                                 "<name>%s</name>"
                                                 "</task>",
                                                 target_task_iterator_uuid (&tasks),
                                                 target_task_iterator_name (&tasks));
                      cleanup_iterator (&tasks);
                      SEND_TO_CLIENT_OR_FAIL ("</tasks>");
                    }

                  SEND_TO_CLIENT_OR_FAIL ("</target>");
                  free (lsc_name);
                }
              cleanup_iterator (&targets);
              SEND_TO_CLIENT_OR_FAIL ("</get_targets_response>");
            }
          get_targets_data_reset (get_targets_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_TASKS:
        {
          task_t task = 0;

          assert (strcasecmp ("GET_TASKS", element_name) == 0);

          if (get_tasks_data->task_id
              && find_task (get_tasks_data->task_id, &task))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_tasks"));
          else if (get_tasks_data->task_id && task == 0)
            {
              if (send_find_error_to_client ("get_tasks",
                                             "task",
                                             get_tasks_data->task_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              gchar* response;
              iterator_t tasks;

              SEND_TO_CLIENT_OR_FAIL ("<get_tasks_response"
                                      " status=\"" STATUS_OK "\""
                                      " status_text=\"" STATUS_OK_TEXT "\">");
              response = g_strdup_printf ("<task_count>%u</task_count>",
                                          task ? 1 : task_count ());
              if (send_to_client (response,
                                  write_to_client,
                                  write_to_client_data))
                {
                  g_free (response);
                  error_send_to_client (error);
                  return;
                }
              g_free (response);

              SENDF_TO_CLIENT_OR_FAIL
               ("<sort>"
                "<field>%s<order>%s</order></field>"
                "</sort>"
                "<apply_overrides>%i</apply_overrides>",
                get_tasks_data->sort_field
                 ? get_tasks_data->sort_field
                 : "ROWID",
                get_tasks_data->sort_order ? "ascending" : "descending",
                get_tasks_data->apply_overrides);

              init_task_iterator (&tasks,
                                  task,
                                  get_tasks_data->sort_order,
                                  get_tasks_data->sort_field);
              while (next (&tasks))
                if (get_tasks_data->details)
                  {
                    /* The detailed version. */

                    int ret, maximum_hosts;
                    gchar *response, *progress_xml;
                    target_t target;
                    char *name, *config, *config_uuid;
                    char *escalator, *escalator_uuid;
                    char *task_target_uuid, *task_target_name, *hosts;
                    char *task_schedule_uuid, *task_schedule_name, *comment;
                    gchar *first_report_id, *first_report;
                    char* description;
                    gchar *description64, *last_report_id, *last_report;
                    gchar *second_last_report_id, *second_last_report;
                    report_t running_report;
                    schedule_t schedule;
                    time_t next_time;
                    task_t task = task_iterator_task (&tasks);

                    target = task_target (task);
                    hosts = target ? target_hosts (target) : NULL;
                    maximum_hosts = hosts ? max_hosts (hosts) : 0;

                    first_report_id = task_first_report_id (task);
                    if (first_report_id)
                      {
                        int debugs, holes, infos, logs, warnings, false_positives;
                        gchar *timestamp;

                        if (report_counts (first_report_id,
                                           &debugs, &holes, &infos, &logs,
                                           &warnings, &false_positives,
                                           get_tasks_data->apply_overrides))
                          /** @todo Either fail better or abort at SQL level. */
                          abort ();

                        if (report_timestamp (first_report_id, &timestamp))
                          /** @todo Either fail better or abort at SQL level. */
                          abort ();

                        first_report = g_strdup_printf ("<first_report>"
                                                        "<report id=\"%s\">"
                                                        "<timestamp>"
                                                        "%s"
                                                        "</timestamp>"
                                                        "<result_count>"
                                                        "<debug>%i</debug>"
                                                        "<hole>%i</hole>"
                                                        "<info>%i</info>"
                                                        "<log>%i</log>"
                                                        "<warning>%i</warning>"
                                                        "<false_positive>"
                                                        "%i"
                                                        "</false_positive>"
                                                        "</result_count>"
                                                        "</report>"
                                                        "</first_report>",
                                                        first_report_id,
                                                        timestamp,
                                                        debugs,
                                                        holes,
                                                        infos,
                                                        logs,
                                                        warnings,
                                                        false_positives);
                        g_free (timestamp);
                        g_free (first_report_id);
                      }
                    else
                      first_report = g_strdup ("");

                    last_report_id = task_last_report_id (task);
                    if (last_report_id)
                      {
                        int debugs, holes, infos, logs, warnings;
                        int false_positives;
                        gchar *timestamp;

                        if (report_counts (last_report_id,
                                           &debugs, &holes, &infos, &logs,
                                           &warnings, &false_positives,
                                           get_tasks_data->apply_overrides))
                          /** @todo Either fail better or abort at SQL level. */
                          abort ();

                        if (report_timestamp (last_report_id, &timestamp))
                          /** @todo Either fail better or abort at SQL level. */
                          abort ();

                        last_report = g_strdup_printf ("<last_report>"
                                                       "<report id=\"%s\">"
                                                       "<timestamp>"
                                                       "%s"
                                                       "</timestamp>"
                                                       "<result_count>"
                                                       "<debug>%i</debug>"
                                                       "<hole>%i</hole>"
                                                       "<info>%i</info>"
                                                       "<log>%i</log>"
                                                       "<warning>%i</warning>"
                                                       "<false_positive>"
                                                       "%i"
                                                       "</false_positive>"
                                                       "</result_count>"
                                                       "</report>"
                                                       "</last_report>",
                                                       last_report_id,
                                                       timestamp,
                                                       debugs,
                                                       holes,
                                                       infos,
                                                       logs,
                                                       warnings,
                                                       false_positives);
                        g_free (timestamp);
                        g_free (last_report_id);
                      }
                    else
                      last_report = g_strdup ("");

                    second_last_report_id = task_second_last_report_id (task);
                    if (second_last_report_id)
                      {
                        int debugs, holes, infos, logs, warnings;
                        int false_positives;
                        gchar *timestamp;

                        if (report_counts (second_last_report_id,
                                           &debugs, &holes, &infos, &logs,
                                           &warnings, &false_positives,
                                           get_tasks_data->apply_overrides))
                          /** @todo Either fail better or abort at SQL level. */
                          abort ();

                        if (report_timestamp (second_last_report_id,
                                              &timestamp))
                          /** @todo Either fail better or abort at SQL level. */
                          abort ();

                        second_last_report = g_strdup_printf
                                              ("<second_last_report>"
                                               "<report id=\"%s\">"
                                               "<timestamp>"
                                               "%s"
                                               "</timestamp>"
                                               "<result_count>"
                                               "<debug>%i</debug>"
                                               "<hole>%i</hole>"
                                               "<info>%i</info>"
                                               "<log>%i</log>"
                                               "<warning>%i</warning>"
                                               "<false_positive>"
                                               "%i"
                                               "</false_positive>"
                                               "</result_count>"
                                               "</report>"
                                               "</second_last_report>",
                                               second_last_report_id,
                                               timestamp,
                                               debugs,
                                               holes,
                                               infos,
                                               logs,
                                               warnings,
                                               false_positives);
                        g_free (timestamp);
                        g_free (second_last_report_id);
                      }
                    else
                      second_last_report = g_strdup ("");

                    running_report = task_current_report (task);
                    if (running_report
                        && report_slave_task_uuid (running_report))
                      progress_xml = g_strdup_printf ("%i",
                                                      report_slave_progress
                                                       (running_report));
                    else if (running_report)
                      {
                        long total = 0;
                        int num_hosts = 0, total_progress;
                        iterator_t hosts;
                        GString *string = g_string_new ("");

                        init_host_iterator (&hosts, running_report, NULL);
                        while (next (&hosts))
                          {
                            unsigned int max_port, current_port;
                            long progress;

                            max_port = host_iterator_max_port (&hosts);
                            current_port = host_iterator_current_port (&hosts);
                            if (max_port)
                              {
                                progress = (current_port * 100) / max_port;
                                if (progress < 0) progress = 0;
                                else if (progress > 100) progress = 100;
                              }
                            else
                              progress = current_port ? 100 : 0;

#if 1
                            tracef ("   attack_state: %s\n", host_iterator_attack_state (&hosts));
                            tracef ("   current_port: %u\n", current_port);
                            tracef ("   max_port: %u\n", max_port);
                            tracef ("   progress for %s: %li\n", host_iterator_host (&hosts), progress);
                            tracef ("   total now: %li\n", total);
#endif
                            total += progress;
                            num_hosts++;

                            g_string_append_printf (string,
                                                    "<host_progress>"
                                                    "<host>%s</host>"
                                                    "%li"
                                                    "</host_progress>",
                                                    host_iterator_host (&hosts),
                                                    progress);
                          }
                        cleanup_iterator (&hosts);

                        total_progress = maximum_hosts
                                         ? (total / maximum_hosts) : 0;

#if 1
                        tracef ("   total: %li\n", total);
                        tracef ("   num_hosts: %i\n", num_hosts);
                        tracef ("   maximum_hosts: %i\n", maximum_hosts);
                        tracef ("   total_progress: %i\n", total_progress);
#endif

                        g_string_append_printf (string,
                                                "%i",
                                                total_progress);
                        progress_xml = g_string_free (string, FALSE);
                      }
                    else
                      progress_xml = g_strdup ("-1");

                    if (get_tasks_data->rcfile)
                      {
                        description = task_description (task);
                        if (description && strlen (description))
                          {
                            gchar *d64;
                            d64 = g_base64_encode ((guchar*) description,
                                                   strlen (description));
                            description64 = g_strdup_printf ("<rcfile>"
                                                             "%s"
                                                             "</rcfile>",
                                                             d64);
                            g_free (d64);
                          }
                        else
                          description64 = g_strdup ("<rcfile></rcfile>");
                        free (description);
                      }
                    else
                      description64 = g_strdup ("");

                    name = task_name (task);
                    comment = task_comment (task);
                    escalator = task_escalator_name (task);
                    escalator_uuid = task_escalator_uuid (task);
                    config = task_config_name (task);
                    config_uuid = task_config_uuid (task);
                    task_target_uuid = target_uuid (target);
                    task_target_name = target_name (target);
                    schedule = task_schedule (task);
                    if (schedule)
                      {
                        task_schedule_uuid = schedule_uuid (schedule);
                        task_schedule_name = schedule_name (schedule);
                      }
                    else
                      {
                        task_schedule_uuid = (char*) g_strdup ("");
                        task_schedule_name = (char*) g_strdup ("");
                      }
                    next_time = task_schedule_next_time (task);
                    response = g_strdup_printf
                                ("<task id=\"%s\">"
                                 "<name>%s</name>"
                                 "<comment>%s</comment>"
                                 "<config id=\"%s\">"
                                 "<name>%s</name>"
                                 "</config>"
                                 "<escalator id=\"%s\">"
                                 "<name>%s</name>"
                                 "</escalator>"
                                 "<target id=\"%s\">"
                                 "<name>%s</name>"
                                 "</target>"
                                 "<status>%s</status>"
                                 "<progress>%s</progress>"
                                 "%s"
                                 "<result_count>"
                                 "<debug>%i</debug>"
                                 "<hole>%i</hole>"
                                 "<info>%i</info>"
                                 "<log>%i</log>"
                                 "<warning>%i</warning>"
                                 "<false_positive>%i</false_positive>"
                                 "</result_count>"
                                 "<report_count>"
                                 "%u<finished>%u</finished>"
                                 "</report_count>"
                                 "<trend>%s</trend>"
                                 "<schedule id=\"%s\">"
                                 "<name>%s</name>"
                                 "<next_time>%s</next_time>"
                                 "</schedule>"
                                 "%s%s%s",
                                 task_iterator_uuid (&tasks),
                                 name,
                                 comment,
                                 config_uuid ? config_uuid : "",
                                 config ? config : "",
                                 escalator_uuid ? escalator_uuid : "",
                                 escalator ? escalator : "",
                                 task_target_uuid ? task_target_uuid : "",
                                 task_target_name ? task_target_name : "",
                                 task_run_status_name (task),
                                 progress_xml,
                                 description64,
                                 task_debugs_size (task),
                                 task_holes_size (task),
                                 task_infos_size (task),
                                 task_logs_size (task),
                                 task_warnings_size (task),
                                 task_false_positive_size (task),
                                 task_report_count (task),
                                 task_finished_report_count (task),
                                 task_trend (task,
                                             get_tasks_data->apply_overrides),
                                 task_schedule_uuid,
                                 task_schedule_name,
                                 (next_time == 0
                                   ? "over"
                                   : ctime_strip_newline (&next_time)),
                                 first_report,
                                 last_report,
                                 second_last_report);
                    free (config);
                    free (escalator);
                    free (task_target_name);
                    g_free (progress_xml);
                    g_free (last_report);
                    g_free (second_last_report);
                    ret = send_to_client (response,
                                          write_to_client,
                                          write_to_client_data);
                    g_free (response);
                    g_free (name);
                    g_free (comment);
                    g_free (description64);
                    free (task_schedule_uuid);
                    free (task_schedule_name);
                    if (ret)
                      {
                        cleanup_iterator (&tasks);
                        error_send_to_client (error);
                        return;
                      }
                    /** @todo Handle error cases.
                     *
                     * The errors are either SQL errors or out of space in
                     * buffer errors.  Both should probably just lead to aborts
                     * at the SQL or buffer output level.
                     */
                    (void) send_reports (task,
                                         get_tasks_data->apply_overrides,
                                         write_to_client,
                                         write_to_client_data);
                    SEND_TO_CLIENT_OR_FAIL ("</task>");
                  }
                else
                  {
                    /* The brief version. */

                    /** @todo This block is very similar to the one above. */

                    task_t index = task_iterator_task (&tasks);
                    gchar *line, *progress_xml;
                    char *name = task_name (index);
                    char *comment = task_comment (index);
                    target_t target;
                    char *tsk_uuid, *config, *config_uuid;
                    char *escalator, *escalator_uuid;
                    char *task_target_uuid, *task_target_name, *hosts;
                    char *task_schedule_uuid, *task_schedule_name;
                    gchar *first_report_id, *first_report;
                    char *description;
                    gchar *description64, *last_report_id, *last_report;
                    gchar *second_last_report_id, *second_last_report;
                    report_t running_report;
                    int maximum_hosts;
                    schedule_t schedule;
                    time_t next_time;

                    /** @todo Buffer entire response so respond with error.
                     *
                     * As above, this is some kind of internal error.  It may
                     * be best to just abort within task_uuid.
                     */
                    if (task_uuid (index, &tsk_uuid)) abort ();

                    target = task_target (index);
                    hosts = target ? target_hosts (target) : NULL;
                    maximum_hosts = hosts ? max_hosts (hosts) : 0;

                    first_report_id = task_first_report_id (index);
                    if (first_report_id)
                      {
                        int debugs, holes, infos, logs, warnings;
                        int false_positives;
                        gchar *timestamp;

                        if (report_counts (first_report_id,
                                           &debugs, &holes, &infos, &logs,
                                           &warnings, &false_positives,
                                           get_tasks_data->apply_overrides))
                          /** @todo Either fail better or abort at SQL level. */
                          abort ();

                        if (report_timestamp (first_report_id, &timestamp))
                          /** @todo Either fail better or abort at SQL level. */
                          abort ();

                        first_report = g_strdup_printf ("<first_report>"
                                                        "<report id=\"%s\">"
                                                        "<timestamp>"
                                                        "%s"
                                                        "</timestamp>"
                                                        "<result_count>"
                                                        "<debug>%i</debug>"
                                                        "<hole>%i</hole>"
                                                        "<info>%i</info>"
                                                        "<log>%i</log>"
                                                        "<warning>%i</warning>"
                                                        "<false_positive>"
                                                        "%i"
                                                        "</false_positive>"
                                                        "</result_count>"
                                                        "</report>"
                                                        "</first_report>",
                                                        first_report_id,
                                                        timestamp,
                                                        debugs,
                                                        holes,
                                                        infos,
                                                        logs,
                                                        warnings,
                                                        false_positives);
                        g_free (timestamp);
                        g_free (first_report_id);
                      }
                    else
                      first_report = g_strdup ("");

                    last_report_id = task_last_report_id (index);
                    if (last_report_id)
                      {
                        int debugs, holes, infos, logs, warnings;
                        int false_positives;
                        gchar *timestamp;

                        if (report_counts (last_report_id,
                                           &debugs, &holes, &infos, &logs,
                                           &warnings, &false_positives,
                                           get_tasks_data->apply_overrides))
                          /** @todo Either fail better or abort at SQL level. */
                          abort ();

                        if (report_timestamp (last_report_id, &timestamp))
                          abort ();

                        last_report = g_strdup_printf ("<last_report>"
                                                       "<report id=\"%s\">"
                                                       "<timestamp>%s</timestamp>"
                                                       "<result_count>"
                                                       "<debug>%i</debug>"
                                                       "<hole>%i</hole>"
                                                       "<info>%i</info>"
                                                       "<log>%i</log>"
                                                       "<warning>%i</warning>"
                                                       "<false_positive>"
                                                       "%i"
                                                       "</false_positive>"
                                                       "</result_count>"
                                                       "</report>"
                                                       "</last_report>",
                                                       last_report_id,
                                                       timestamp,
                                                       debugs,
                                                       holes,
                                                       infos,
                                                       logs,
                                                       warnings,
                                                       false_positives);
                        g_free (timestamp);
                        g_free (last_report_id);
                      }
                    else
                      last_report = g_strdup ("");

                    if (get_tasks_data->rcfile)
                      {
                        description = task_description (index);
                        if (description && strlen (description))
                          {
                            gchar *d64;
                            d64 = g_base64_encode ((guchar*) description,
                                                   strlen (description));
                            description64 = g_strdup_printf ("<rcfile>"
                                                             "%s"
                                                             "</rcfile>",
                                                             d64);
                            g_free (d64);
                          }
                        else
                          description64 = g_strdup ("<rcfile></rcfile>");
                        free (description);
                      }
                    else
                      description64 = g_strdup ("");

                    second_last_report_id = task_second_last_report_id (index);
                    if (second_last_report_id)
                      {
                        int debugs, holes, infos, logs, warnings;
                        int false_positives;
                        gchar *timestamp;

                        if (report_counts (second_last_report_id,
                                           &debugs, &holes, &infos, &logs,
                                           &warnings, &false_positives,
                                           get_tasks_data->apply_overrides))
                          /** @todo Either fail better or abort at SQL level. */
                          abort ();

                        if (report_timestamp (second_last_report_id, &timestamp))
                          abort ();

                        second_last_report = g_strdup_printf
                                              ("<second_last_report>"
                                               "<report id=\"%s\">"
                                               "<timestamp>%s</timestamp>"
                                               "<result_count>"
                                               "<debug>%i</debug>"
                                               "<hole>%i</hole>"
                                               "<info>%i</info>"
                                               "<log>%i</log>"
                                               "<warning>%i</warning>"
                                               "<false_positive>"
                                               "%i"
                                               "</false_positive>"
                                               "</result_count>"
                                               "</report>"
                                               "</second_last_report>",
                                               second_last_report_id,
                                               timestamp,
                                               debugs,
                                               holes,
                                               infos,
                                               logs,
                                               warnings,
                                               false_positives);
                        g_free (timestamp);
                        g_free (second_last_report_id);
                      }
                    else
                      second_last_report = g_strdup ("");

                    running_report = task_current_report (index);
                    if (running_report
                        && report_slave_task_uuid (running_report))
                      progress_xml = g_strdup_printf ("%i",
                                                      report_slave_progress
                                                       (running_report));
                    else if (running_report)
                      {
                        long total = 0;
                        int num_hosts = 0, total_progress;
                        iterator_t hosts;
                        GString *string = g_string_new ("");

                        init_host_iterator (&hosts, running_report, NULL);
                        while (next (&hosts))
                          {
                            unsigned int max_port, current_port;
                            long progress;

                            max_port = host_iterator_max_port (&hosts);
                            current_port = host_iterator_current_port (&hosts);
                            if (max_port)
                              {
                                progress = (current_port * 100) / max_port;
                                if (progress < 0) progress = 0;
                                else if (progress > 100) progress = 100;
                              }
                            else
                              progress = current_port ? 100 : 0;
                            total += progress;
                            num_hosts++;

#if 1
                            tracef ("   attack_state: %s\n", host_iterator_attack_state (&hosts));
                            tracef ("   current_port: %u\n", current_port);
                            tracef ("   max_port: %u\n", max_port);
                            tracef ("   progress for %s: %li\n", host_iterator_host (&hosts), progress);
                            tracef ("   total now: %li\n", total);
#endif

                            g_string_append_printf (string,
                                                    "<host_progress>"
                                                    "<host>%s</host>"
                                                    "%li"
                                                    "</host_progress>",
                                                    host_iterator_host (&hosts),
                                                    progress);
                          }
                        cleanup_iterator (&hosts);

                        total_progress = maximum_hosts ? (total / maximum_hosts) : 0;

#if 1
                        tracef ("   total: %li\n", total);
                        tracef ("   num_hosts: %i\n", num_hosts);
                        tracef ("   maximum_hosts: %i\n", maximum_hosts);
                        tracef ("   total_progress: %i\n", total_progress);
#endif

                        g_string_append_printf (string,
                                                "%i",
                                                total_progress);
                        progress_xml = g_string_free (string, FALSE);
                      }
                    else
                      progress_xml = g_strdup ("-1");

                    config = task_config_name (index);
                    config_uuid = task_config_uuid (index);
                    escalator = task_escalator_name (index);
                    escalator_uuid = task_escalator_uuid (index);
                    task_target_uuid = target_uuid (target);
                    task_target_name = target_name (target);
                    schedule = task_schedule (index);
                    if (schedule)
                      {
                        task_schedule_uuid = schedule_uuid (schedule);
                        task_schedule_name = schedule_name (schedule);
                      }
                    else
                      {
                        task_schedule_uuid = (char*) g_strdup ("");
                        task_schedule_name = (char*) g_strdup ("");
                      }
                    next_time = task_schedule_next_time (index);
                    line = g_strdup_printf ("<task"
                                            " id=\"%s\">"
                                            "<name>%s</name>"
                                            "<comment>%s</comment>"
                                            "<config id=\"%s\">"
                                            "<name>%s</name>"
                                            "</config>"
                                            "<escalator id=\"%s\">"
                                            "<name>%s</name>"
                                            "</escalator>"
                                            "<target id=\"%s\">"
                                            "<name>%s</name>"
                                            "</target>"
                                            "<status>%s</status>"
                                            "<progress>%s</progress>"
                                            "%s"
                                            "<result_count>"
                                            "<debug>%i</debug>"
                                            "<hole>%i</hole>"
                                            "<info>%i</info>"
                                            "<log>%i</log>"
                                            "<warning>%i</warning>"
                                            "<false_positive>"
                                            "%i"
                                            "</false_positive>"
                                            "</result_count>"
                                            "<report_count>"
                                            "%u<finished>%u</finished>"
                                            "</report_count>"
                                            "<trend>%s</trend>"
                                            "<schedule id=\"%s\">"
                                            "<name>%s</name>"
                                            "<next_time>%s</next_time>"
                                            "</schedule>"
                                            "%s%s%s"
                                            "</task>",
                                            tsk_uuid,
                                            name,
                                            comment,
                                            config_uuid ? config_uuid : "",
                                            config ? config : "",
                                            escalator_uuid ? escalator_uuid : "",
                                            escalator ? escalator : "",
                                            task_target_uuid ? task_target_uuid : "",
                                            task_target_name ? task_target_name : "",
                                            task_run_status_name (index),
                                            progress_xml,
                                            description64,
                                            task_debugs_size (index),
                                            task_holes_size (index),
                                            task_infos_size (index),
                                            task_logs_size (index),
                                            task_warnings_size (index),
                                            task_false_positive_size (index),
                                            task_report_count (index),
                                            task_finished_report_count (index),
                                            task_trend
                                             (index,
                                              get_tasks_data->apply_overrides),
                                            task_schedule_uuid,
                                            task_schedule_name,
                                            (next_time == 0
                                              ? "over"
                                              : ctime_strip_newline (&next_time)),
                                            first_report,
                                            last_report,
                                            second_last_report);
                    free (config);
                    free (escalator);
                    free (escalator_uuid);
                    free (task_target_name);
                    g_free (progress_xml);
                    g_free (last_report);
                    g_free (second_last_report);
                    free (name);
                    free (comment);
                    g_free (description64);
                    free (tsk_uuid);
                    free (task_schedule_uuid);
                    free (task_schedule_name);
                    if (send_to_client (line,
                                        write_to_client,
                                        write_to_client_data))
                      {
                        g_free (line);
                        cleanup_iterator (&tasks);
                        error_send_to_client (error);
                        cleanup_iterator (&tasks);
                        return;
                      }
                    g_free (line);
                  }
              cleanup_iterator (&tasks);
              SEND_TO_CLIENT_OR_FAIL ("</get_tasks_response>");
            }
          }

        get_tasks_data_reset (get_tasks_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_VERIFY_AGENT:
        assert (strcasecmp ("VERIFY_AGENT", element_name) == 0);
        if (verify_agent_data->agent_id)
          {
            agent_t agent;

            if (find_agent (verify_agent_data->agent_id, &agent))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("verify_agent"));
            else if (agent == 0)
              {
                if (send_find_error_to_client
                     ("verify_agent",
                      "report format",
                      verify_agent_data->agent_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (verify_agent (agent))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("verify_agent"));
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("verify_agent",
                                      "Attempt to verify a hidden report"
                                      " format"));
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("verify_agent"));
                  break;
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("verify_agent",
                              "VERIFY_AGENT requires a agent_id"
                              " attribute"));
        verify_agent_data_reset (verify_agent_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_VERIFY_REPORT_FORMAT:
        assert (strcasecmp ("VERIFY_REPORT_FORMAT", element_name) == 0);
        if (verify_report_format_data->report_format_id)
          {
            report_format_t report_format;

            if (find_report_format (verify_report_format_data->report_format_id,
                                    &report_format))
              SEND_TO_CLIENT_OR_FAIL
               (XML_INTERNAL_ERROR ("verify_report_format"));
            else if (report_format == 0)
              {
                if (send_find_error_to_client
                     ("verify_report_format",
                      "report format",
                      verify_report_format_data->report_format_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (verify_report_format (report_format))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("verify_report_format"));
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("verify_report_format",
                                      "Attempt to verify a hidden report"
                                      " format"));
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("verify_report_format"));
                  break;
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("verify_report_format",
                              "VERIFY_REPORT_FORMAT requires a report_format_id"
                              " attribute"));
        verify_report_format_data_reset (verify_report_format_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      default:
        assert (0);
        break;
    }
}

/**
 * @brief Handle the addition of text to an OMP XML element.
 *
 * React to the addition of text to the value of an XML element.
 * React according to the current value of \ref client_state,
 * usually appending the text to some part of the current task
 * with functions like openvas_append_text,
 * \ref add_task_description_line and \ref append_to_task_comment.
 *
 * @param[in]  context           Parser context.
 * @param[in]  text              The text.
 * @param[in]  text_len          Length of the text.
 * @param[in]  user_data         Dummy parameter.
 * @param[in]  error             Error parameter.
 */
static void
omp_xml_handle_text (/*@unused@*/ GMarkupParseContext* context,
                     const gchar *text,
                     gsize text_len,
                     /*@unused@*/ gpointer user_data,
                     /*@unused@*/ GError **error)
{
  if (text_len == 0) return;
  tracef ("   XML   text: %s\n", text);
  switch (client_state)
    {
      case CLIENT_AUTHENTICATE_CREDENTIALS_USERNAME:
        append_to_credentials_username (&current_credentials, text, text_len);
        break;
      case CLIENT_AUTHENTICATE_CREDENTIALS_PASSWORD:
        append_to_credentials_password (&current_credentials, text, text_len);
        break;

      case CLIENT_MODIFY_CONFIG_NVT_SELECTION_FAMILY:
        openvas_append_text (&modify_config_data->nvt_selection_family,
                             text,
                             text_len);
        break;

      case CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_ALL:
        openvas_append_text
         (&modify_config_data->family_selection_family_all_text,
          text,
          text_len);
        break;
      case CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_GROWING:
        openvas_append_text
         (&modify_config_data->family_selection_family_growing_text,
          text,
          text_len);
        break;
      case CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_NAME:
        openvas_append_text (&modify_config_data->family_selection_family_name,
                             text,
                             text_len);
        break;
      case CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_GROWING:
        openvas_append_text (&modify_config_data->family_selection_growing_text,
                             text,
                             text_len);
        break;

      case CLIENT_MODIFY_CONFIG_PREFERENCE_NAME:
        openvas_append_text (&modify_config_data->preference_name,
                             text,
                             text_len);
        break;
      case CLIENT_MODIFY_CONFIG_PREFERENCE_VALUE:
        openvas_append_text (&modify_config_data->preference_value,
                             text,
                             text_len);
        break;

      case CLIENT_MODIFY_REPORT_COMMENT:
        openvas_append_text (&modify_report_data->comment,
                             text,
                             text_len);
        break;

      case CLIENT_MODIFY_REPORT_FORMAT_NAME:
        openvas_append_text (&modify_report_format_data->name,
                             text,
                             text_len);
        break;
      case CLIENT_MODIFY_REPORT_FORMAT_SUMMARY:
        openvas_append_text (&modify_report_format_data->summary,
                             text,
                             text_len);
        break;

      case CLIENT_MODIFY_TASK_COMMENT:
        openvas_append_text (&modify_task_data->comment, text, text_len);
        break;
      case CLIENT_MODIFY_TASK_NAME:
        openvas_append_text (&modify_task_data->name, text, text_len);
        break;
      case CLIENT_MODIFY_TASK_RCFILE:
        openvas_append_text (&modify_task_data->rcfile, text, text_len);
        break;
      case CLIENT_MODIFY_TASK_FILE:
        openvas_append_text (&modify_task_data->file, text, text_len);
        break;

      case CLIENT_CREATE_AGENT_COMMENT:
        openvas_append_text (&create_agent_data->comment, text, text_len);
        break;
      case CLIENT_CREATE_AGENT_HOWTO_INSTALL:
        openvas_append_text (&create_agent_data->howto_install, text, text_len);
        break;
      case CLIENT_CREATE_AGENT_HOWTO_USE:
        openvas_append_text (&create_agent_data->howto_use, text, text_len);
        break;
      case CLIENT_CREATE_AGENT_INSTALLER:
        openvas_append_text (&create_agent_data->installer, text, text_len);
        break;
      case CLIENT_CREATE_AGENT_INSTALLER_FILENAME:
        openvas_append_text (&create_agent_data->installer_filename,
                             text,
                             text_len);
        break;
      case CLIENT_CREATE_AGENT_INSTALLER_SIGNATURE:
        openvas_append_text (&create_agent_data->installer_signature,
                             text,
                             text_len);
        break;
      case CLIENT_CREATE_AGENT_NAME:
        openvas_append_text (&create_agent_data->name, text, text_len);
        break;

      case CLIENT_CREATE_CONFIG_COMMENT:
        openvas_append_text (&create_config_data->comment, text, text_len);
        break;
      case CLIENT_CREATE_CONFIG_COPY:
        openvas_append_text (&create_config_data->copy, text, text_len);
        break;
      case CLIENT_CREATE_CONFIG_NAME:
        openvas_append_text (&create_config_data->name, text, text_len);
        break;
      case CLIENT_CREATE_CONFIG_RCFILE:
        openvas_append_text (&create_config_data->rcfile, text, text_len);
        break;

      case CLIENT_C_C_GCR_CONFIG_COMMENT:
        openvas_append_text (&(import_config_data->comment),
                             text,
                             text_len);
        break;
      case CLIENT_C_C_GCR_CONFIG_NAME:
        openvas_append_text (&(import_config_data->name),
                             text,
                             text_len);
        break;
      case CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_INCLUDE:
        openvas_append_text (&(import_config_data->nvt_selector_include),
                             text,
                             text_len);
        break;
      case CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_NAME:
        openvas_append_text (&(import_config_data->nvt_selector_name),
                             text,
                             text_len);
        break;
      case CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_TYPE:
        openvas_append_text (&(import_config_data->nvt_selector_type),
                             text,
                             text_len);
        break;
      case CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_FAMILY_OR_NVT:
        openvas_append_text (&(import_config_data->nvt_selector_family_or_nvt),
                             text,
                             text_len);
        break;
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_ALT:
        openvas_append_text (&(import_config_data->preference_alt),
                             text,
                             text_len);
        break;
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NAME:
        openvas_append_text (&(import_config_data->preference_name),
                             text,
                             text_len);
        break;
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NVT_NAME:
        openvas_append_text (&(import_config_data->preference_nvt_name),
                             text,
                             text_len);
        break;
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_TYPE:
        openvas_append_text (&(import_config_data->preference_type),
                             text,
                             text_len);
        break;
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_VALUE:
        openvas_append_text (&(import_config_data->preference_value),
                             text,
                             text_len);
        break;

      case CLIENT_CREATE_LSC_CREDENTIAL_COMMENT:
        openvas_append_text (&create_lsc_credential_data->comment,
                             text,
                             text_len);
        break;
      case CLIENT_CREATE_LSC_CREDENTIAL_LOGIN:
        openvas_append_text (&create_lsc_credential_data->login,
                             text,
                             text_len);
        break;
      case CLIENT_CREATE_LSC_CREDENTIAL_NAME:
        openvas_append_text (&create_lsc_credential_data->name,
                             text,
                             text_len);
        break;
      case CLIENT_CREATE_LSC_CREDENTIAL_PASSWORD:
        openvas_append_text (&create_lsc_credential_data->password,
                             text,
                             text_len);
        break;

      case CLIENT_CREATE_ESCALATOR_COMMENT:
        openvas_append_text (&create_escalator_data->comment, text, text_len);
        break;
      case CLIENT_CREATE_ESCALATOR_CONDITION:
        openvas_append_text (&create_escalator_data->condition, text, text_len);
        break;
      case CLIENT_CREATE_ESCALATOR_EVENT:
        openvas_append_text (&create_escalator_data->event, text, text_len);
        break;
      case CLIENT_CREATE_ESCALATOR_METHOD:
        openvas_append_text (&create_escalator_data->method, text, text_len);
        break;
      case CLIENT_CREATE_ESCALATOR_NAME:
        openvas_append_text (&create_escalator_data->name, text, text_len);
        break;

      case CLIENT_CREATE_ESCALATOR_CONDITION_DATA:
        openvas_append_text (&create_escalator_data->part_data, text, text_len);
        break;
      case CLIENT_CREATE_ESCALATOR_EVENT_DATA:
        openvas_append_text (&create_escalator_data->part_data, text, text_len);
        break;
      case CLIENT_CREATE_ESCALATOR_METHOD_DATA:
        openvas_append_text (&create_escalator_data->part_data, text, text_len);
        break;

      case CLIENT_CREATE_ESCALATOR_CONDITION_DATA_NAME:
        openvas_append_text (&create_escalator_data->part_name, text, text_len);
        break;
      case CLIENT_CREATE_ESCALATOR_EVENT_DATA_NAME:
        openvas_append_text (&create_escalator_data->part_name, text, text_len);
        break;
      case CLIENT_CREATE_ESCALATOR_METHOD_DATA_NAME:
        openvas_append_text (&create_escalator_data->part_name, text, text_len);
        break;

      case CLIENT_CREATE_NOTE_HOSTS:
        openvas_append_text (&create_note_data->hosts, text, text_len);
        break;
      case CLIENT_CREATE_NOTE_PORT:
        openvas_append_text (&create_note_data->port, text, text_len);
        break;
      case CLIENT_CREATE_NOTE_TEXT:
        openvas_append_text (&create_note_data->text, text, text_len);
        break;
      case CLIENT_CREATE_NOTE_THREAT:
        openvas_append_text (&create_note_data->threat, text, text_len);
        break;

      case CLIENT_CREATE_OVERRIDE_HOSTS:
        openvas_append_text (&create_override_data->hosts, text, text_len);
        break;
      case CLIENT_CREATE_OVERRIDE_NEW_THREAT:
        openvas_append_text (&create_override_data->new_threat, text, text_len);
        break;
      case CLIENT_CREATE_OVERRIDE_PORT:
        openvas_append_text (&create_override_data->port, text, text_len);
        break;
      case CLIENT_CREATE_OVERRIDE_TEXT:
        openvas_append_text (&create_override_data->text, text, text_len);
        break;
      case CLIENT_CREATE_OVERRIDE_THREAT:
        openvas_append_text (&create_override_data->threat, text, text_len);
        break;

      case CLIENT_CRF_GRFR_REPORT_FORMAT_CONTENT_TYPE:
        openvas_append_text (&create_report_format_data->content_type,
                             text,
                             text_len);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_DESCRIPTION:
        openvas_append_text (&create_report_format_data->description,
                             text,
                             text_len);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_EXTENSION:
        openvas_append_text (&create_report_format_data->extension,
                             text,
                             text_len);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_FILE:
        openvas_append_text (&create_report_format_data->file, text, text_len);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_GLOBAL:
        openvas_append_text (&create_report_format_data->global,
                             text,
                             text_len);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_NAME:
        openvas_append_text (&create_report_format_data->name, text, text_len);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_NAME:
        openvas_append_text (&create_report_format_data->param_name,
                             text,
                             text_len);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_VALUE:
        openvas_append_text (&create_report_format_data->param_value,
                             text,
                             text_len);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_SIGNATURE:
        openvas_append_text (&create_report_format_data->signature,
                             text,
                             text_len);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_SUMMARY:
        openvas_append_text (&create_report_format_data->summary,
                             text,
                             text_len);
        break;
      case CLIENT_CRF_GRFR_REPORT_FORMAT_TRUST:
        break;

      case CLIENT_CREATE_SCHEDULE_COMMENT:
        openvas_append_text (&create_schedule_data->comment, text, text_len);
        break;
      case CLIENT_CREATE_SCHEDULE_DURATION:
        openvas_append_text (&create_schedule_data->duration, text, text_len);
        break;
      case CLIENT_CREATE_SCHEDULE_DURATION_UNIT:
        openvas_append_text (&create_schedule_data->duration_unit,
                             text,
                             text_len);
        break;
      case CLIENT_CREATE_SCHEDULE_FIRST_TIME_DAY_OF_MONTH:
        openvas_append_text (&create_schedule_data->first_time_day_of_month,
                             text,
                             text_len);
        break;
      case CLIENT_CREATE_SCHEDULE_FIRST_TIME_HOUR:
        openvas_append_text (&create_schedule_data->first_time_hour,
                             text,
                             text_len);
        break;
      case CLIENT_CREATE_SCHEDULE_FIRST_TIME_MINUTE:
        openvas_append_text (&create_schedule_data->first_time_minute,
                             text,
                             text_len);
        break;
      case CLIENT_CREATE_SCHEDULE_FIRST_TIME_MONTH:
        openvas_append_text (&create_schedule_data->first_time_month,
                             text,
                             text_len);
        break;
      case CLIENT_CREATE_SCHEDULE_FIRST_TIME_YEAR:
        openvas_append_text (&create_schedule_data->first_time_year,
                             text,
                             text_len);
        break;
      case CLIENT_CREATE_SCHEDULE_NAME:
        openvas_append_text (&create_schedule_data->name, text, text_len);
        break;
      case CLIENT_CREATE_SCHEDULE_PERIOD:
        openvas_append_text (&create_schedule_data->period, text, text_len);
        break;
      case CLIENT_CREATE_SCHEDULE_PERIOD_UNIT:
        openvas_append_text (&create_schedule_data->period_unit,
                             text,
                             text_len);
        break;

      case CLIENT_CREATE_SLAVE_COMMENT:
        openvas_append_text (&create_slave_data->comment, text, text_len);
        break;
      case CLIENT_CREATE_SLAVE_HOST:
        openvas_append_text (&create_slave_data->host, text, text_len);
        break;
      case CLIENT_CREATE_SLAVE_LOGIN:
        openvas_append_text (&create_slave_data->login, text, text_len);
        break;
      case CLIENT_CREATE_SLAVE_NAME:
        openvas_append_text (&create_slave_data->name, text, text_len);
        break;
      case CLIENT_CREATE_SLAVE_PASSWORD:
        openvas_append_text (&create_slave_data->password, text, text_len);
        break;
      case CLIENT_CREATE_SLAVE_PORT:
        openvas_append_text (&create_slave_data->port, text, text_len);
        break;

      case CLIENT_CREATE_TARGET_COMMENT:
        openvas_append_text (&create_target_data->comment, text, text_len);
        break;
      case CLIENT_CREATE_TARGET_HOSTS:
        openvas_append_text (&create_target_data->hosts, text, text_len);
        break;
      case CLIENT_CREATE_TARGET_NAME:
        openvas_append_text (&create_target_data->name, text, text_len);
        break;
      case CLIENT_CREATE_TARGET_TARGET_LOCATOR:
        openvas_append_text (&create_target_data->target_locator, text, text_len);
        break;
      case CLIENT_CREATE_TARGET_TARGET_LOCATOR_PASSWORD:
        openvas_append_text (&create_target_data->target_locator_password,
                             text,
                             text_len);
        break;
      case CLIENT_CREATE_TARGET_TARGET_LOCATOR_USERNAME:
        openvas_append_text (&create_target_data->target_locator_username,
                             text,
                             text_len);
        break;

      case CLIENT_CREATE_TASK_COMMENT:
        append_to_task_comment (create_task_data->task, text, text_len);
        break;
      case CLIENT_CREATE_TASK_NAME:
        append_to_task_name (create_task_data->task, text, text_len);
        break;
      case CLIENT_CREATE_TASK_RCFILE:
        /* Append the text to the task description. */
        add_task_description_line (create_task_data->task,
                                   text,
                                   text_len);
        break;

      case CLIENT_MODIFY_NOTE_HOSTS:
        openvas_append_text (&modify_note_data->hosts, text, text_len);
        break;
      case CLIENT_MODIFY_NOTE_PORT:
        openvas_append_text (&modify_note_data->port, text, text_len);
        break;
      case CLIENT_MODIFY_NOTE_TEXT:
        openvas_append_text (&modify_note_data->text, text, text_len);
        break;
      case CLIENT_MODIFY_NOTE_THREAT:
        openvas_append_text (&modify_note_data->threat, text, text_len);
        break;

      case CLIENT_MODIFY_OVERRIDE_HOSTS:
        openvas_append_text (&modify_override_data->hosts, text, text_len);
        break;
      case CLIENT_MODIFY_OVERRIDE_NEW_THREAT:
        openvas_append_text (&modify_override_data->new_threat, text, text_len);
        break;
      case CLIENT_MODIFY_OVERRIDE_PORT:
        openvas_append_text (&modify_override_data->port, text, text_len);
        break;
      case CLIENT_MODIFY_OVERRIDE_TEXT:
        openvas_append_text (&modify_override_data->text, text, text_len);
        break;
      case CLIENT_MODIFY_OVERRIDE_THREAT:
        openvas_append_text (&modify_override_data->threat, text, text_len);
        break;

      default:
        /* Just pass over the text. */
        break;
    }
}

/**
 * @brief Handle an OMP XML parsing error.
 *
 * Simply leave the error for the caller of the parser to handle.
 *
 * @param[in]  context           Parser context.
 * @param[in]  error             The error.
 * @param[in]  user_data         Dummy parameter.
 */
static void
omp_xml_handle_error (/*@unused@*/ GMarkupParseContext* context,
                      GError *error,
                      /*@unused@*/ gpointer user_data)
{
  tracef ("   XML ERROR %s\n", error->message);
}


/* OMP input processor. */

/** @todo Most likely the client should get these from init_omp_process
 *        inside an omp_parser_t and should pass the omp_parser_t to
 *        process_omp_client_input.  process_omp_client_input can pass then
 *        pass them on to the other Manager "libraries". */
extern char from_client[];
extern buffer_size_t from_client_start;
extern buffer_size_t from_client_end;

/**
 * @brief Initialise OMP library.
 *
 * @param[in]  log_config      Logging configuration list.
 * @param[in]  nvt_cache_mode  True when running in NVT caching mode.
 * @param[in]  database        Location of manage database.
 *
 * @return 0 success, -1 error, -2 database is wrong version, -3 database
 *         needs to be initialized from server.
 */
int
init_omp (GSList *log_config, int nvt_cache_mode, const gchar *database)
{
  g_log_set_handler (G_LOG_DOMAIN,
                     ALL_LOG_LEVELS,
                     (GLogFunc) openvas_log_func,
                     log_config);
  command_data_init (&command_data);
  return init_manage (log_config, nvt_cache_mode, database);
}

/**
 * @brief Initialise OMP library data for a process.
 *
 * @param[in]  update_nvt_cache  0 operate normally, -1 just update NVT cache,
 *                               -2 just rebuild NVT cache.
 * @param[in]  database          Location of manage database.
 * @param[in]  write_to_client       Function to write to client.
 * @param[in]  write_to_client_data  Argument to \p write_to_client.
 *
 * This should run once per process, before the first call to \ref
 * process_omp_client_input.
 */
void
init_omp_process (int update_nvt_cache, const gchar *database,
                  int (*write_to_client) (void*), void* write_to_client_data)
{
  forked = 0;
  init_manage_process (update_nvt_cache, database);
  /* Create the XML parser. */
  xml_parser.start_element = omp_xml_handle_start_element;
  xml_parser.end_element = omp_xml_handle_end_element;
  xml_parser.text = omp_xml_handle_text;
  xml_parser.passthrough = NULL;
  xml_parser.error = omp_xml_handle_error;
  if (xml_context) g_free (xml_context);
  xml_context = g_markup_parse_context_new
                 (&xml_parser,
                  0,
                  omp_parser_new (write_to_client, write_to_client_data),
                  (GDestroyNotify) omp_parser_free);
}

/**
 * @brief Process any XML available in \ref from_client.
 *
 * \if STATIC
 *
 * Call the XML parser and let the callback functions do the work
 * (\ref omp_xml_handle_start_element, \ref omp_xml_handle_end_element,
 * \ref omp_xml_handle_text and \ref omp_xml_handle_error).
 *
 * The callback functions will queue any resulting scanner commands in
 * \ref to_scanner (using \ref send_to_server) and any replies for
 * the client in \ref to_client (using \ref send_to_client).
 *
 * \endif
 *
 * @todo The -2 return has been replaced by send_to_client trying to write
 *       the to_client buffer to the client when it is full.  This is
 *       necessary, as the to_client buffer may fill up halfway through the
 *       processing of an OMP element.
 *
 * @return 0 success, -1 error, -2 or -3 too little space in \ref to_client
 *         or the scanner output buffer (respectively), -4 XML syntax error.
 */
int
process_omp_client_input ()
{
  gboolean success;
  GError* error = NULL;

  /* In the XML parser handlers all writes to the to_scanner buffer must be
   * complete OTP commands, because the caller may also write into to_scanner
   * between calls to this function (via manage_check_current_task). */

  if (xml_context == NULL) return -1;

  current_error = 0;
  success = g_markup_parse_context_parse (xml_context,
                                          from_client + from_client_start,
                                          from_client_end - from_client_start,
                                          &error);
  if (success == FALSE)
    {
      int err;
      if (error)
        {
          err = -4;
          if (g_error_matches (error,
                               G_MARKUP_ERROR,
                               G_MARKUP_ERROR_UNKNOWN_ELEMENT))
            tracef ("   client error: G_MARKUP_ERROR_UNKNOWN_ELEMENT\n");
          else if (g_error_matches (error,
                                    G_MARKUP_ERROR,
                                    G_MARKUP_ERROR_INVALID_CONTENT))
            {
              if (current_error)
                {
                  /* This is the return status for a forked child. */
                  forked = 2; /* Prevent further forking. */
                  g_error_free (error);
                  return current_error;
                }
              tracef ("   client error: G_MARKUP_ERROR_INVALID_CONTENT\n");
            }
          else if (g_error_matches (error,
                                    G_MARKUP_ERROR,
                                    G_MARKUP_ERROR_UNKNOWN_ATTRIBUTE))
            tracef ("   client error: G_MARKUP_ERROR_UNKNOWN_ATTRIBUTE\n");
          else
            err = -1;
          g_message ("   Failed to parse client XML: %s\n", error->message);
          g_error_free (error);
        }
      else
        err = -1;
      /* In all error cases the caller must cease to call this function as it
       * would be too hard, if possible at all, to figure out the position of
       * start of the next command. */
      return err;
    }
  from_client_end = from_client_start = 0;
  if (forked)
    return 3;
  return 0;
}

/**
 * @brief Return whether the scanner is active.
 *
 * @return 1 if the scanner is doing something that the manager
 *         must wait for, else 0.
 */
short
scanner_is_active ()
{
  return scanner_active;
}


/* OMP change processor. */

/**
 * @brief Deal with any changes caused by other processes.
 *
 * @return 0 success, 1 did something, -1 too little space in the scanner
 *         output buffer.
 */
int
process_omp_change ()
{
  return manage_check_current_task ();
}
