/* Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/sql_plugin_var.h"

#include <limits.h>
#include <string>
#include <unordered_map>
#include <utility>

#include "m_string.h"
#include "map_helpers.h"
#include "my_dbug.h"
#include "my_list.h"
#include "mysql/psi/mysql_mutex.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/current_thd.h"
#include "sql/item.h"
#include "sql/mysqld.h"
#include "sql/psi_memory_key.h"
#include "sql/set_var.h"
#include "sql/sql_class.h"     // THD
#include "sql/sql_const.h"
#include "sql/sql_plugin.h"
#include "sql/strfunc.h"       // find_type
#include "sql/sys_vars_shared.h" // intern_find_sys_var
#include "sql/system_variables.h"
#include "sql_string.h"

/**
  Set value for global variable with PLUGIN_VAR_MEMALLOC flag.

  @param[in]     thd   Thread context.
  @param[in]     var   Plugin variable.
  @param[in,out] dest  Destination memory pointer.
  @param[in]     value '\0'-terminated new value.

  @return Completion status
  @retval false Success
  @retval true  Failure
*/

bool plugin_var_memalloc_global_update(THD *thd,
                                       st_mysql_sys_var *var,
                                       char **dest, const char *value)
{
  char *old_value= *dest;
  DBUG_EXECUTE_IF("simulate_bug_20292712", my_sleep(1000););
  DBUG_ENTER("plugin_var_memalloc_global_update");

  if (value && !(value= my_strdup(key_memory_global_system_variables,
                                  value, MYF(MY_WME))))
    DBUG_RETURN(true);

  var->update(thd, var, (void **) dest, (const void *) &value);

  if (old_value)
    my_free(old_value);

  DBUG_RETURN(false);
}

/**
  Set value for thread local variable with PLUGIN_VAR_MEMALLOC flag.

  @param[in]     thd   Thread context.
  @param[in]     var   Plugin variable.
  @param[in,out] dest  Destination memory pointer.
  @param[in]     value '\0'-terminated new value.

  Most plugin variable values are stored on dynamic_variables_ptr.
  Releasing memory occupied by these values is as simple as freeing
  dynamic_variables_ptr.

  An exception to the rule are PLUGIN_VAR_MEMALLOC variables, which
  are stored on individual memory hunks. All of these hunks has to
  be freed when it comes to cleanup.

  It may happen that a plugin was uninstalled and descriptors of
  it's variables are lost. In this case it is impossible to locate
  corresponding values.

  In addition to allocating and setting variable value, new element
  is added to dynamic_variables_allocs list. When thread is done, it
  has to call plugin_var_memalloc_free() to release memory used by
  PLUGIN_VAR_MEMALLOC variables.

  If var is NULL, variable update function is not called. This is
  needed when we take snapshot of system variables during thread
  initialization.

  @note List element and variable value are stored on the same memory
  hunk. List element is followed by variable value.

  @return Completion status
  @retval false Success
  @retval true  Failure
*/

bool plugin_var_memalloc_session_update(THD *thd,
                                        st_mysql_sys_var *var,
                                        char **dest, const char *value)

{
  LIST *old_element= NULL;
  struct System_variables *vars= &thd->variables;
  DBUG_ENTER("plugin_var_memalloc_session_update");

  if (value)
  {
    size_t length= strlen(value) + 1;
    LIST *element;
    if (!(element= (LIST *) my_malloc(key_memory_THD_variables,
                                      sizeof(LIST) + length, MYF(MY_WME))))
      DBUG_RETURN(true);
    memcpy(element + 1, value, length);
    value= (const char *) (element + 1);
    vars->dynamic_variables_allocs= list_add(vars->dynamic_variables_allocs,
                                             element);
  }

  if (*dest)
    old_element= (LIST *) (*dest - sizeof(LIST));

  if (var)
    var->update(thd, var, (void **) dest, (const void *) &value);
  else
    *dest= (char *) value;

  if (old_element)
  {
    vars->dynamic_variables_allocs= list_delete(vars->dynamic_variables_allocs,
                                                old_element);
    my_free(old_element);
  }
  DBUG_RETURN(false);
}

SHOW_TYPE pluginvar_show_type(st_mysql_sys_var *plugin_var)
{
  switch (plugin_var->flags & PLUGIN_VAR_TYPEMASK) {
  case PLUGIN_VAR_BOOL:
    return SHOW_MY_BOOL;
  case PLUGIN_VAR_INT:
    return SHOW_INT;
  case PLUGIN_VAR_LONG:
    return SHOW_LONG;
  case PLUGIN_VAR_LONGLONG:
    return SHOW_LONGLONG;
  case PLUGIN_VAR_STR:
    return SHOW_CHAR_PTR;
  case PLUGIN_VAR_ENUM:
  case PLUGIN_VAR_SET:
    return SHOW_CHAR;
  case PLUGIN_VAR_DOUBLE:
    return SHOW_DOUBLE;
  default:
    DBUG_ASSERT(0);
    return SHOW_UNDEF;
  }
}

/*
  returns a pointer to the memory which holds the thd-local variable or
  a pointer to the global variable if thd==null.
  If required, will sync with global variables if the requested variable
  has not yet been allocated in the current thread.
*/
uchar *intern_sys_var_ptr(THD* thd, int offset, bool global_lock)
{
  DBUG_ASSERT(offset >= 0);
  DBUG_ASSERT((uint)offset <= global_system_variables.dynamic_variables_head);

  if (!thd)
    return (uchar*) global_system_variables.dynamic_variables_ptr + offset;

  /*
    dynamic_variables_head points to the largest valid offset
  */
  if (!thd->variables.dynamic_variables_ptr ||
      (uint)offset > thd->variables.dynamic_variables_head)
  {
    /* Current THD only. Don't trigger resync on remote THD. */
    if (current_thd == thd)
      alloc_and_copy_thd_dynamic_variables(thd, global_lock);
    else
      return (uchar*) global_system_variables.dynamic_variables_ptr + offset;
  }

  return (uchar*)thd->variables.dynamic_variables_ptr + offset;
}

/****************************************************************************
  Value type thunks, allows the C world to play in the C++ world
****************************************************************************/

int item_value_type(st_mysql_value *value)
{
  switch (((st_item_value_holder*)value)->item->result_type()) {
  case INT_RESULT:
    return MYSQL_VALUE_TYPE_INT;
  case REAL_RESULT:
    return MYSQL_VALUE_TYPE_REAL;
  default:
    return MYSQL_VALUE_TYPE_STRING;
  }
}

const char *item_val_str(st_mysql_value *value,
                                char *buffer, int *length)
{
  String str(buffer, *length, system_charset_info), *res;
  if (!(res= ((st_item_value_holder*)value)->item->val_str(&str)))
    return NULL;
  *length= static_cast<int>(res->length());
  if (res->c_ptr_quick() == buffer)
    return buffer;

  /*
    Lets be nice and create a temporary string since the
    buffer was too small
  */
  return current_thd->strmake(res->c_ptr_quick(), res->length());
}


int item_val_int(st_mysql_value *value, long long *buf)
{
  Item *item= ((st_item_value_holder*)value)->item;
  *buf= item->val_int();
  if (item->is_null())
    return 1;
  return 0;
}

int item_is_unsigned(st_mysql_value *value)
{
  Item *item= ((st_item_value_holder*)value)->item;
  return item->unsigned_flag;
}

int item_val_real(st_mysql_value *value, double *buf)
{
  Item *item= ((st_item_value_holder*)value)->item;
  *buf= item->val_real();
  if (item->is_null())
    return 1;
  return 0;
}


bool sys_var_pluginvar::check_update_type(Item_result type)
{
  switch (plugin_var->flags & PLUGIN_VAR_TYPEMASK) {
  case PLUGIN_VAR_INT:
  case PLUGIN_VAR_LONG:
  case PLUGIN_VAR_LONGLONG:
    return type != INT_RESULT;
  case PLUGIN_VAR_STR:
    return type != STRING_RESULT;
  case PLUGIN_VAR_ENUM:
  case PLUGIN_VAR_BOOL:
  case PLUGIN_VAR_SET:
    return type != STRING_RESULT && type != INT_RESULT;
  case PLUGIN_VAR_DOUBLE:
    return type != INT_RESULT && type != REAL_RESULT && type != DECIMAL_RESULT;
  default:
    return true;
  }
}


uchar* sys_var_pluginvar::real_value_ptr(THD *thd, enum_var_type type)
{
  DBUG_ASSERT(thd || (type == OPT_GLOBAL));
  if (plugin_var->flags & PLUGIN_VAR_THDLOCAL)
  {
    if (type == OPT_GLOBAL)
      thd= NULL;

    return intern_sys_var_ptr(thd, *(int*) (plugin_var+1), false);
  }
  return *(uchar**) (plugin_var+1);
}


TYPELIB* sys_var_pluginvar::plugin_var_typelib(void)
{
  switch (plugin_var->flags & (PLUGIN_VAR_TYPEMASK | PLUGIN_VAR_THDLOCAL)) {
  case PLUGIN_VAR_ENUM:
    return ((sysvar_enum_t *)plugin_var)->typelib;
  case PLUGIN_VAR_SET:
    return ((sysvar_set_t *)plugin_var)->typelib;
  case PLUGIN_VAR_ENUM | PLUGIN_VAR_THDLOCAL:
    return ((thdvar_enum_t *)plugin_var)->typelib;
  case PLUGIN_VAR_SET | PLUGIN_VAR_THDLOCAL:
    return ((thdvar_set_t *)plugin_var)->typelib;
  default:
    return NULL;
  }
  return NULL;	/* Keep compiler happy */
}


uchar* sys_var_pluginvar::do_value_ptr(THD *running_thd, THD *target_thd, enum_var_type type,
                                       LEX_STRING*)
{
  uchar* result;

  result= real_value_ptr(target_thd, type);

  if ((plugin_var->flags & PLUGIN_VAR_TYPEMASK) == PLUGIN_VAR_ENUM)
    result= (uchar*) get_type(plugin_var_typelib(), *(ulong*)result);
  else if ((plugin_var->flags & PLUGIN_VAR_TYPEMASK) == PLUGIN_VAR_SET)
    result= (uchar*) set_to_string(running_thd, 0, *(ulonglong*) result,
                                   plugin_var_typelib()->type_names);
  return result;
}

bool sys_var_pluginvar::do_check(THD *thd, set_var *var)
{
  st_item_value_holder value;
  DBUG_ASSERT(plugin_var->check);

  value.value_type= item_value_type;
  value.val_str= item_val_str;
  value.val_int= item_val_int;
  value.val_real= item_val_real;
  value.is_unsigned= item_is_unsigned;
  value.item= var->value;

  return plugin_var->check(thd, plugin_var, &var->save_result, &value);
}

bool sys_var_pluginvar::session_update(THD *thd, set_var *var)
{
  bool rc= false;
  DBUG_ASSERT(!is_readonly());
  DBUG_ASSERT(plugin_var->flags & PLUGIN_VAR_THDLOCAL);
  DBUG_ASSERT(thd == current_thd);

  mysql_mutex_lock(&LOCK_global_system_variables);
  void *tgt= real_value_ptr(thd, var->type);
  const void *src= var->value ? (void*)&var->save_result
                              : (void*)real_value_ptr(thd, OPT_GLOBAL);
  mysql_mutex_unlock(&LOCK_global_system_variables);

  if ((plugin_var->flags & PLUGIN_VAR_TYPEMASK) == PLUGIN_VAR_STR &&
      plugin_var->flags & PLUGIN_VAR_MEMALLOC)
    rc= plugin_var_memalloc_session_update(thd, plugin_var, (char **) tgt,
                                           *(const char **) src);
  else
    plugin_var->update(thd, plugin_var, tgt, src);

  return rc;
}

bool sys_var_pluginvar::global_update(THD *thd, set_var *var)
{
  bool rc= false;
  DBUG_ASSERT(!is_readonly());
  mysql_mutex_assert_owner(&LOCK_global_system_variables);

  void *tgt= real_value_ptr(thd, var->type);
  const void *src= &var->save_result;

  if (!var->value)
  {
    switch (plugin_var->flags & (PLUGIN_VAR_TYPEMASK | PLUGIN_VAR_THDLOCAL)) {
    case PLUGIN_VAR_INT:
      src= &((sysvar_uint_t*) plugin_var)->def_val;
      break;
    case PLUGIN_VAR_LONG:
      src= &((sysvar_ulong_t*) plugin_var)->def_val;
      break;
    case PLUGIN_VAR_LONGLONG:
      src= &((sysvar_ulonglong_t*) plugin_var)->def_val;
      break;
    case PLUGIN_VAR_ENUM:
      src= &((sysvar_enum_t*) plugin_var)->def_val;
      break;
    case PLUGIN_VAR_SET:
      src= &((sysvar_set_t*) plugin_var)->def_val;
      break;
    case PLUGIN_VAR_BOOL:
      src= &((sysvar_bool_t*) plugin_var)->def_val;
      break;
    case PLUGIN_VAR_STR:
      src= &((sysvar_str_t*) plugin_var)->def_val;
      break;
    case PLUGIN_VAR_DOUBLE:
      src= &((sysvar_double_t*) plugin_var)->def_val;
      break;
    case PLUGIN_VAR_INT | PLUGIN_VAR_THDLOCAL:
      src= &((thdvar_uint_t*) plugin_var)->def_val;
      break;
    case PLUGIN_VAR_LONG | PLUGIN_VAR_THDLOCAL:
      src= &((thdvar_ulong_t*) plugin_var)->def_val;
      break;
    case PLUGIN_VAR_LONGLONG | PLUGIN_VAR_THDLOCAL:
      src= &((thdvar_ulonglong_t*) plugin_var)->def_val;
      break;
    case PLUGIN_VAR_ENUM | PLUGIN_VAR_THDLOCAL:
      src= &((thdvar_enum_t*) plugin_var)->def_val;
      break;
    case PLUGIN_VAR_SET | PLUGIN_VAR_THDLOCAL:
      src= &((thdvar_set_t*) plugin_var)->def_val;
      break;
    case PLUGIN_VAR_BOOL | PLUGIN_VAR_THDLOCAL:
      src= &((thdvar_bool_t*) plugin_var)->def_val;
      break;
    case PLUGIN_VAR_STR | PLUGIN_VAR_THDLOCAL:
      src= &((thdvar_str_t*) plugin_var)->def_val;
      break;
    case PLUGIN_VAR_DOUBLE | PLUGIN_VAR_THDLOCAL:
      src= &((thdvar_double_t*) plugin_var)->def_val;
      break;
    default:
      DBUG_ASSERT(0);
    }
  }

  if ((plugin_var->flags & PLUGIN_VAR_TYPEMASK) == PLUGIN_VAR_STR &&
      plugin_var->flags & PLUGIN_VAR_MEMALLOC)
    rc= plugin_var_memalloc_global_update(thd, plugin_var, (char **) tgt,
                                          *(const char **) src);
  else
    plugin_var->update(thd, plugin_var, tgt, src);

  return rc;
}

bool sys_var_pluginvar::is_default(THD *thd, set_var *var)
{
  void *tgt= real_value_ptr(thd, var->type);

  switch (plugin_var->flags & (PLUGIN_VAR_TYPEMASK | PLUGIN_VAR_THDLOCAL))
  {
    case PLUGIN_VAR_INT:
      return (((sysvar_uint_t*) plugin_var)->def_val == *(uint *)tgt);
    case PLUGIN_VAR_LONG:
      return (((sysvar_ulong_t*) plugin_var)->def_val == *(ulong *)tgt);
    case PLUGIN_VAR_LONGLONG:
      return
        (((sysvar_ulonglong_t*) plugin_var)->def_val == *(ulonglong *)tgt);
    case PLUGIN_VAR_ENUM:
      return (((sysvar_enum_t*) plugin_var)->def_val == *(ulong *)tgt);
    case PLUGIN_VAR_SET:
      return (((sysvar_set_t*) plugin_var)->def_val == *(ulong *)tgt);
    case PLUGIN_VAR_BOOL:
      return (((sysvar_bool_t*) plugin_var)->def_val == *(bool *)tgt);
    case PLUGIN_VAR_STR:
      return
        !strcmp((char*)(((sysvar_str_t*) plugin_var)->def_val),*(char **)tgt);
    case PLUGIN_VAR_DOUBLE:
      return (((sysvar_double_t*) plugin_var)->def_val == *(double *)tgt);
    case PLUGIN_VAR_INT | PLUGIN_VAR_THDLOCAL:
      return (((thdvar_uint_t*) plugin_var)->def_val == *(uint *)tgt);
    case PLUGIN_VAR_LONG | PLUGIN_VAR_THDLOCAL:
      return (((thdvar_ulong_t*) plugin_var)->def_val == *(ulong *)tgt);
    case PLUGIN_VAR_LONGLONG | PLUGIN_VAR_THDLOCAL:
      return
        (((thdvar_ulonglong_t*) plugin_var)->def_val == *(ulonglong *)tgt);
    case PLUGIN_VAR_ENUM | PLUGIN_VAR_THDLOCAL:
      return (((thdvar_enum_t*) plugin_var)->def_val == *(ulong *)tgt);
    case PLUGIN_VAR_SET | PLUGIN_VAR_THDLOCAL:
      return (((thdvar_set_t*) plugin_var)->def_val == *(ulong *)tgt);
    case PLUGIN_VAR_BOOL | PLUGIN_VAR_THDLOCAL:
      return (((thdvar_bool_t*) plugin_var)->def_val == *(bool *)tgt);
    case PLUGIN_VAR_STR | PLUGIN_VAR_THDLOCAL:
      return
        !strcmp((char*)(((thdvar_str_t*) plugin_var)->def_val),*(char **)tgt);
    case PLUGIN_VAR_DOUBLE | PLUGIN_VAR_THDLOCAL:
      return (((thdvar_double_t*) plugin_var)->def_val == *(double *)tgt);
  }
  return 0;
}

longlong sys_var_pluginvar::get_min_value()
{
  switch (plugin_var->flags & (PLUGIN_VAR_TYPEMASK | PLUGIN_VAR_THDLOCAL))
  {
    case PLUGIN_VAR_INT:
      return ((sysvar_uint_t*) plugin_var)->min_val;
    case PLUGIN_VAR_LONG:
      return ((sysvar_ulong_t*) plugin_var)->min_val;
    case PLUGIN_VAR_LONGLONG:
      return ((sysvar_ulonglong_t*) plugin_var)->min_val;
    case PLUGIN_VAR_DOUBLE:
      return ((sysvar_double_t*) plugin_var)->min_val;
    case PLUGIN_VAR_INT | PLUGIN_VAR_THDLOCAL:
      return ((thdvar_uint_t*) plugin_var)->min_val;
    case PLUGIN_VAR_LONG | PLUGIN_VAR_THDLOCAL:
      return ((thdvar_ulong_t*) plugin_var)->min_val;
    case PLUGIN_VAR_LONGLONG | PLUGIN_VAR_THDLOCAL:
      return ((thdvar_ulonglong_t*) plugin_var)->min_val;
    case PLUGIN_VAR_DOUBLE | PLUGIN_VAR_THDLOCAL:
      return ((thdvar_double_t*) plugin_var)->min_val;
  }
  return 0;
}

ulonglong sys_var_pluginvar::get_max_value()
{
  switch (plugin_var->flags & (PLUGIN_VAR_TYPEMASK | PLUGIN_VAR_THDLOCAL))
  {
    case PLUGIN_VAR_INT:
      return ((sysvar_uint_t*) plugin_var)->max_val;
    case PLUGIN_VAR_LONG:
      return ((sysvar_ulong_t*) plugin_var)->max_val;
    case PLUGIN_VAR_LONGLONG:
      return ((sysvar_ulonglong_t*) plugin_var)->max_val;
    case PLUGIN_VAR_DOUBLE:
      return ((sysvar_double_t*) plugin_var)->max_val;
    case PLUGIN_VAR_INT | PLUGIN_VAR_THDLOCAL:
      return ((thdvar_uint_t*) plugin_var)->max_val;
    case PLUGIN_VAR_LONG | PLUGIN_VAR_THDLOCAL:
      return ((thdvar_ulong_t*) plugin_var)->max_val;
    case PLUGIN_VAR_LONGLONG | PLUGIN_VAR_THDLOCAL:
      return ((thdvar_ulonglong_t*) plugin_var)->max_val;
    case PLUGIN_VAR_DOUBLE | PLUGIN_VAR_THDLOCAL:
      return ((thdvar_double_t*) plugin_var)->max_val;
  }
  return 0;
}

/**
  Enforce the NO DEFAULT policy for plugin system variables

  A plugin variable does not explicitly call the plugin supplied check function
  when setting the default value, e.g. SET @<plugin_var@> = DEFAULT.

  But when the PLUGIN_VAR_NODEFAULT is set setting the default value is
  prohibited.
  This function gets called after the actual check done by
  sys_var_pluginvar::do_check() so it does not need to check again.

  it only needs to enforce the PLUGIN_VAR_NODEFAULT flag.

  There's no need for special error hence just returning true is enough.

  @sa sys_var::on_check_function, sys_var::check,
    sys_var_pluginvar::do_check(), PLUGIN_VAR_NODEFAULT

  @param self   the sys_var structure for the variable being set
  @param var    the data about the value being set
  @return is the setting valid
  @retval true not valid
  @retval false valid
*/
bool sys_var_pluginvar::on_check_pluginvar(sys_var *self MY_ATTRIBUTE((unused)),
                                           THD*, set_var *var)
{
  /* This handler is installed only if NO_DEFAULT is specified */
  DBUG_ASSERT(((sys_var_pluginvar *) self)->plugin_var->flags &
              PLUGIN_VAR_NODEFAULT);

  return (!var->value);
}


/****************************************************************************
  default variable data check and update functions
****************************************************************************/

int check_func_bool(THD*, st_mysql_sys_var*,
                           void *save, st_mysql_value *value)
{
  char buff[STRING_BUFFER_USUAL_SIZE];
  const char *str;
  int result, length;
  long long tmp;

  if (value->value_type(value) == MYSQL_VALUE_TYPE_STRING)
  {
    length= sizeof(buff);
    if (!(str= value->val_str(value, buff, &length)) ||
        (result= find_type(&bool_typelib, str, length, 1)-1) < 0)
      goto err;
  }
  else
  {
    if (value->val_int(value, &tmp) < 0)
      goto err;
    if (tmp > 1)
      goto err;
    result= (int) tmp;
  }
  *(bool *) save= result ? TRUE : FALSE;
  return 0;
err:
  return 1;
}


int check_func_int(THD *thd, st_mysql_sys_var *var,
                          void *save, st_mysql_value *value)
{
  bool fixed1, fixed2;
  long long orig, val;
  struct my_option options;
  value->val_int(value, &orig);
  val= orig;
  plugin_opt_set_limits(&options, var);

  if (var->flags & PLUGIN_VAR_UNSIGNED)
  {
    if ((fixed1= (!value->is_unsigned(value) && val < 0)))
      val=0;
    *(uint *)save= (uint) getopt_ull_limit_value((ulonglong) val, &options,
                                                   &fixed2);
  }
  else
  {
    if ((fixed1= (value->is_unsigned(value) && val < 0)))
      val=LLONG_MAX;
    *(int *)save= (int) getopt_ll_limit_value(val, &options, &fixed2);
  }

  return throw_bounds_warning(thd, var->name, fixed1 || fixed2,
                              value->is_unsigned(value), orig);
}


int check_func_long(THD *thd, st_mysql_sys_var *var,
                          void *save, st_mysql_value *value)
{
  bool fixed1, fixed2;
  long long orig, val;
  struct my_option options;
  value->val_int(value, &orig);
  val= orig;
  plugin_opt_set_limits(&options, var);

  if (var->flags & PLUGIN_VAR_UNSIGNED)
  {
    if ((fixed1= (!value->is_unsigned(value) && val < 0)))
      val=0;
    *(ulong *)save= (ulong) getopt_ull_limit_value((ulonglong) val, &options,
                                                   &fixed2);
  }
  else
  {
    if ((fixed1= (value->is_unsigned(value) && val < 0)))
      val=LLONG_MAX;
    *(long *)save= (long) getopt_ll_limit_value(val, &options, &fixed2);
  }

  return throw_bounds_warning(thd, var->name, fixed1 || fixed2,
                              value->is_unsigned(value), orig);
}


int check_func_longlong(THD *thd, st_mysql_sys_var *var,
                               void *save, st_mysql_value *value)
{
  bool fixed1, fixed2;
  long long orig, val;
  struct my_option options;
  value->val_int(value, &orig);
  val= orig;
  plugin_opt_set_limits(&options, var);

  if (var->flags & PLUGIN_VAR_UNSIGNED)
  {
    if ((fixed1= (!value->is_unsigned(value) && val < 0)))
      val=0;
    *(ulonglong *)save= getopt_ull_limit_value((ulonglong) val, &options,
                                               &fixed2);
  }
  else
  {
    if ((fixed1= (value->is_unsigned(value) && val < 0)))
      val=LLONG_MAX;
    *(longlong *)save= getopt_ll_limit_value(val, &options, &fixed2);
  }

  return throw_bounds_warning(thd, var->name, fixed1 || fixed2,
                              value->is_unsigned(value), orig);
}

int check_func_str(THD *thd, st_mysql_sys_var*,
                          void *save, st_mysql_value *value)
{
  char buff[STRING_BUFFER_USUAL_SIZE];
  const char *str;
  int length;

  length= sizeof(buff);
  if ((str= value->val_str(value, buff, &length)))
    str= thd->strmake(str, length);
  *(const char**)save= str;
  return 0;
}


int check_func_enum(THD*, st_mysql_sys_var *var,
                           void *save, st_mysql_value *value)
{
  char buff[STRING_BUFFER_USUAL_SIZE];
  const char *str;
  TYPELIB *typelib;
  long long tmp;
  long result;
  int length;

  if (var->flags & PLUGIN_VAR_THDLOCAL)
    typelib= ((thdvar_enum_t*) var)->typelib;
  else
    typelib= ((sysvar_enum_t*) var)->typelib;

  if (value->value_type(value) == MYSQL_VALUE_TYPE_STRING)
  {
    length= sizeof(buff);
    if (!(str= value->val_str(value, buff, &length)))
      goto err;
    if ((result= (long)find_type(typelib, str, length, 0) - 1) < 0)
      goto err;
  }
  else
  {
    if (value->val_int(value, &tmp))
      goto err;
    if (tmp < 0 || tmp >= static_cast<long long>(typelib->count))
      goto err;
    result= (long) tmp;
  }
  *(long*)save= result;
  return 0;
err:
  return 1;
}


int check_func_set(THD*, st_mysql_sys_var *var,
                          void *save, st_mysql_value *value)
{
  char buff[STRING_BUFFER_USUAL_SIZE], *error= 0;
  const char *str;
  TYPELIB *typelib;
  ulonglong result;
  uint error_len= 0;                            // init as only set on error
  bool not_used;
  int length;

  if (var->flags & PLUGIN_VAR_THDLOCAL)
    typelib= ((thdvar_set_t*) var)->typelib;
  else
    typelib= ((sysvar_set_t*)var)->typelib;

  if (value->value_type(value) == MYSQL_VALUE_TYPE_STRING)
  {
    length= sizeof(buff);
    if (!(str= value->val_str(value, buff, &length)))
      goto err;
    result= find_set(typelib, str, length, NULL,
                     &error, &error_len, &not_used);
    if (error_len)
      goto err;
  }
  else
  {
    if (value->val_int(value, (long long *)&result))
      goto err;
    if (unlikely((result >= (1ULL << typelib->count)) &&
                 (typelib->count < sizeof(long)*8)))
      goto err;
  }
  *(ulonglong*)save= result;
  return 0;
err:
  return 1;
}

int check_func_double(THD *thd, st_mysql_sys_var *var,
                             void *save, st_mysql_value *value)
{
  double v;
  bool fixed;
  struct my_option option;

  value->val_real(value, &v);
  plugin_opt_set_limits(&option, var);
  *(double *) save= getopt_double_limit_value(v, &option, &fixed);

  return throw_bounds_warning(thd, var->name, fixed, v);
}


void update_func_bool(THD*, st_mysql_sys_var*,
                             void *tgt, const void *save)
{
  *(bool *) tgt= *(bool *) save ? TRUE : FALSE;
}


void update_func_int(THD*, st_mysql_sys_var*,
                            void *tgt, const void *save)
{
  *(int *)tgt= *(int *) save;
}


void update_func_long(THD*, st_mysql_sys_var*,
                             void *tgt, const void *save)
{
  *(long *)tgt= *(long *) save;
}


void update_func_longlong(THD*, st_mysql_sys_var*,
                                 void *tgt, const void *save)
{
  *(longlong *)tgt= *(ulonglong *) save;
}


void update_func_str(THD*, st_mysql_sys_var*,
                             void *tgt, const void *save)
{
  *(char **) tgt= *(char **) save;
}

void update_func_double(THD*, st_mysql_sys_var*,
                               void *tgt, const void *save)
{
  *(double *) tgt= *(double *) save;
}

/*
  called by register_var, construct_options and test_plugin_options.
  Returns the 'bookmark' for the named variable.
  LOCK_system_variables_hash should be at least read locked
*/
st_bookmark *find_bookmark(const char *plugin, const char *name,
                                  int flags)
{
  size_t namelen, length, pluginlen= 0;
  char *varname, *p;

  if (!(flags & PLUGIN_VAR_THDLOCAL))
    return NULL;

  namelen= strlen(name);
  if (plugin)
    pluginlen= strlen(plugin) + 1;
  length= namelen + pluginlen + 2;
  varname= (char*) my_alloca(length);

  if (plugin)
  {
    strxmov(varname + 1, plugin, "_", name, NullS);
    for (p= varname + 1; *p; p++)
      if (*p == '-')
        *p= '_';
  }
  else
    memcpy(varname + 1, name, namelen + 1);

  varname[0]= flags & PLUGIN_VAR_TYPEMASK;

  const auto it= get_bookmark_hash()->find(std::string(varname, length - 1));
  if (it == get_bookmark_hash()->end())
    return nullptr;
  else
    return it->second;
}

void plugin_opt_set_limits(struct my_option *options,
                           const st_mysql_sys_var *opt)
{
  switch (opt->flags & (PLUGIN_VAR_TYPEMASK |
                        PLUGIN_VAR_UNSIGNED | PLUGIN_VAR_THDLOCAL)) {
  /* global system variables */
  case PLUGIN_VAR_INT:
    OPTION_SET_LIMITS(GET_INT, options, (sysvar_int_t*) opt);
    break;
  case PLUGIN_VAR_INT | PLUGIN_VAR_UNSIGNED:
    OPTION_SET_LIMITS(GET_UINT, options, (sysvar_uint_t*) opt);
    break;
  case PLUGIN_VAR_LONG:
    OPTION_SET_LIMITS(GET_LONG, options, (sysvar_long_t*) opt);
    break;
  case PLUGIN_VAR_LONG | PLUGIN_VAR_UNSIGNED:
    OPTION_SET_LIMITS(GET_ULONG, options, (sysvar_ulong_t*) opt);
    break;
  case PLUGIN_VAR_LONGLONG:
    OPTION_SET_LIMITS(GET_LL, options, (sysvar_longlong_t*) opt);
    break;
  case PLUGIN_VAR_LONGLONG | PLUGIN_VAR_UNSIGNED:
    OPTION_SET_LIMITS(GET_ULL, options, (sysvar_ulonglong_t*) opt);
    break;
  case PLUGIN_VAR_ENUM:
    options->var_type= GET_ENUM;
    options->typelib= ((sysvar_enum_t*) opt)->typelib;
    options->def_value= ((sysvar_enum_t*) opt)->def_val;
    options->min_value= options->block_size= 0;
    options->max_value= options->typelib->count - 1;
    break;
  case PLUGIN_VAR_SET:
    options->var_type= GET_SET;
    options->typelib= ((sysvar_set_t*) opt)->typelib;
    options->def_value= ((sysvar_set_t*) opt)->def_val;
    options->min_value= options->block_size= 0;
    options->max_value= (1ULL << options->typelib->count) - 1;
    break;
  case PLUGIN_VAR_BOOL:
    options->var_type= GET_BOOL;
    options->def_value= ((sysvar_bool_t*) opt)->def_val;
    break;
  case PLUGIN_VAR_STR:
    options->var_type= ((opt->flags & PLUGIN_VAR_MEMALLOC) ?
                        GET_STR_ALLOC : GET_STR);
    options->def_value= (intptr) ((sysvar_str_t*) opt)->def_val;
    break;
  case PLUGIN_VAR_DOUBLE:
    OPTION_SET_LIMITS_DOUBLE(options, (sysvar_double_t*) opt);
    break;
  /* threadlocal variables */
  case PLUGIN_VAR_INT | PLUGIN_VAR_THDLOCAL:
    OPTION_SET_LIMITS(GET_INT, options, (thdvar_int_t*) opt);
    break;
  case PLUGIN_VAR_INT | PLUGIN_VAR_UNSIGNED | PLUGIN_VAR_THDLOCAL:
    OPTION_SET_LIMITS(GET_UINT, options, (thdvar_uint_t*) opt);
    break;
  case PLUGIN_VAR_LONG | PLUGIN_VAR_THDLOCAL:
    OPTION_SET_LIMITS(GET_LONG, options, (thdvar_long_t*) opt);
    break;
  case PLUGIN_VAR_LONG | PLUGIN_VAR_UNSIGNED | PLUGIN_VAR_THDLOCAL:
    OPTION_SET_LIMITS(GET_ULONG, options, (thdvar_ulong_t*) opt);
    break;
  case PLUGIN_VAR_LONGLONG | PLUGIN_VAR_THDLOCAL:
    OPTION_SET_LIMITS(GET_LL, options, (thdvar_longlong_t*) opt);
    break;
  case PLUGIN_VAR_LONGLONG | PLUGIN_VAR_UNSIGNED | PLUGIN_VAR_THDLOCAL:
    OPTION_SET_LIMITS(GET_ULL, options, (thdvar_ulonglong_t*) opt);
    break;
  case PLUGIN_VAR_DOUBLE | PLUGIN_VAR_THDLOCAL:
    OPTION_SET_LIMITS_DOUBLE(options, (thdvar_double_t*) opt);
    break;
  case PLUGIN_VAR_ENUM | PLUGIN_VAR_THDLOCAL:
    options->var_type= GET_ENUM;
    options->typelib= ((thdvar_enum_t*) opt)->typelib;
    options->def_value= ((thdvar_enum_t*) opt)->def_val;
    options->min_value= options->block_size= 0;
    options->max_value= options->typelib->count - 1;
    break;
  case PLUGIN_VAR_SET | PLUGIN_VAR_THDLOCAL:
    options->var_type= GET_SET;
    options->typelib= ((thdvar_set_t*) opt)->typelib;
    options->def_value= ((thdvar_set_t*) opt)->def_val;
    options->min_value= options->block_size= 0;
    options->max_value= (1ULL << options->typelib->count) - 1;
    break;
  case PLUGIN_VAR_BOOL | PLUGIN_VAR_THDLOCAL:
    options->var_type= GET_BOOL;
    options->def_value= ((thdvar_bool_t*) opt)->def_val;
    break;
  case PLUGIN_VAR_STR | PLUGIN_VAR_THDLOCAL:
    options->var_type= ((opt->flags & PLUGIN_VAR_MEMALLOC) ?
                        GET_STR_ALLOC : GET_STR);
    options->def_value= (intptr) ((thdvar_str_t*) opt)->def_val;
    break;
  default:
    DBUG_ASSERT(0);
  }
  options->arg_type= REQUIRED_ARG;
  if (opt->flags & PLUGIN_VAR_NOCMDARG)
    options->arg_type= NO_ARG;
  if (opt->flags & PLUGIN_VAR_OPCMDARG)
    options->arg_type= OPT_ARG;
}
