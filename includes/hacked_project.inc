<?php

/**
 * Ensapsulates a Hacked! project.
 *
 * This class should handle all the complexity for you, and so you should be able to do:
 * <code>
 * $project = hackedProject('context');
 * $project->compute_differences();
 * </code>
 *
 * Which is quite nice I think.
 */
class hackedProject {
  var $name = '';

  var $project_info = array();

  var $remote_files_downloader;

  var $remote_files;
  var $local_files;

  var $project_type = '';
  var $existing_version = '';

  var $result = array();

  var $project_identified = FALSE;
  var $remote_downloaded = FALSE;
  var $remote_hashed = FALSE;
  var $local_hashed = FALSE;

  /**
   * Constructor.
   */
  function hackedProject($name) {
    $this->name = $name;
    $this->remote_files_downloader = new hackedProjectWebFilesDownloader($this);
  }

  /**
   * Get the Human readable title of this project.
   */
  function title() {
    $this->identify_project();
    return isset($this->project_info['title']) ? $this->project_info['title'] : $this->name;
  }

  /**
   * Identify the project from the name we've been created with.
   *
   * We leverage the update (status) module to get the data we require about
   * projects. We just pull the information in, and make descisions about this
   * project being from GIT or not.
   */
  function identify_project() {
    // Only do this once, no matter how many times we're called.
    if (!empty($this->project_identified)) {
      return;
    }

    // Fetch the required data from the update (status) module.
    // TODO: clean this up.
    $available = update_get_available(TRUE);
    $data = update_calculate_project_data($available);
    $releases = _update_get_cached_available_releases();
    
    foreach ($data as $key => $project) {
      if ($key == $this->name) {
        $this->project_info = $project;
        if (!isset($this->project_info['releases']) || !is_array($this->project_info['releases'])) {
          $this->project_info['releases'] = array();
        }
        if (isset($releases[$key]['releases']) && is_array($releases[$key]['releases'])) {
          $this->project_info['releases'] += $releases[$key]['releases'];
        }

        // Add in the additional info that update module strips out.
        // This is a really naff way of doing this, but update (status) module
        // ripped out a lot of useful stuff in issue:
        // http://drupal.org/node/669554

        // Find an item that this project includes:
        if (hacked_git_enabled()) {
          foreach ($project['includes'] as $name => $title) {
            if (is_dir(drupal_get_path($project['project_type'], $name) . '/.git')) {
              $this->remote_files_downloader = new hackedProjectWebGITDownloader($this, drupal_get_filename($project['project_type'], $name));
              break;
            }
          }
        }
        elseif (hacked_cvs_enabled()) {
          foreach ($project['includes'] as $name => $title) {
            if (is_dir(drupal_get_path($project['project_type'], $name) . '/CVS')) {
              $this->remote_files_downloader = new hackedProjectWebCVSDownloader($this, drupal_get_filename($project['project_type'], $name));
              break;
            }
          }
        }

        $this->project_identified = TRUE;
        $this->existing_version = $this->project_info['existing_version'];
        $this->project_type = $this->project_info['project_type'];
        break;
      }
    }

    // Logging.
    if (!$this->project_identified) {
      watchdog('hacked', 'Could not identify project: @name', array('@name' => $this->name), WATCHDOG_WARNING);
    }
  }

  /**
   * Downloads the remote project to be hashed later.
   */
  function download_remote_project() {
    // Only do this once, no matter how many times we're called.
    if (!empty($this->remote_downloaded)) {
      return;
    }

    $this->identify_project();
    $this->remote_downloaded = (bool)$this->remote_files_downloader->download();

    // Logging.
    if (!$this->remote_downloaded) {
      watchdog('hacked', 'Could not download project: @title', array('@title' => $this->title()), WATCHDOG_ERROR);
    }
  }

  /**
   * Hashes the remote project downloaded earlier.
   */
  function hash_remote_project() {
    // Only do this once, no matter how many times we're called.
    if (!empty($this->remote_hashed)) {
      return;
    }

    // Ensure that the remote project has actually been downloaded.
    $this->download_remote_project();

    // Set up the remote file group.
    $base_path = $this->remote_files_downloader->get_final_destination();
    $this->remote_files = hackedFileGroup::fromDirectory($base_path);
    $this->remote_files->compute_hashes();

    $this->remote_hashed = !empty($this->remote_files->files);

    // Logging.
    if (!$this->remote_hashed) {
      watchdog('hacked', 'Could not hash remote project: @title', array('@title' => $this->title()), WATCHDOG_ERROR);
    }
  }

  /**
   * Locate the base directory of the local project.
   */
  function locate_local_project() {
    // we need a remote project to do this :(
    $this->hash_remote_project();

    // Do we have at least some modules to check for:
    if (!is_array($this->project_info['includes']) || !count($this->project_info['includes'])) {
      return FALSE;
    }

    // If this project is drupal it, we need to handle it specially
    if ($this->project_type != 'core') {
      $includes = array_keys($this->project_info['includes']);
      $include = array_shift($includes);
      $include_type = $this->project_info['project_type'];
    }
    else {
      // Just use the system module to find where we've installed drupal
      $include = 'system';
      $include_type = 'module';
    }

    //$include = 'image_captcha';

    $path = drupal_get_path($include_type, $include);

    // Now we need to find the path of the info file in the downloaded package:
    $temp = '';
    foreach ($this->remote_files->files as $file) {
      if (preg_match('@(^|.*/)' . $include . '.info$@', $file)) {
        $temp = $file;
        break;
      }
    }

    // How many '/' were in that path:
    $slash_count = substr_count($temp, '/');
    $back_track = str_repeat('/..', $slash_count);

    return realpath($path . $back_track);
  }

  /**
   * Hash the local version of the project.
   */
  function hash_local_project() {
    // Only do this once, no matter how many times we're called.
    if (!empty($this->local_hashed)) {
      return;
    }

    $location = $this->locate_local_project();

    $this->local_files = hackedFileGroup::fromList($location, $this->remote_files->files);
    $this->local_files->compute_hashes();

    $this->local_hashed = !empty($this->local_files->files);

    // Logging.
    if (!$this->local_hashed) {
      watchdog('hacked', 'Could not hash local project: @title', array('@title' => $this->title()), WATCHDOG_ERROR);
    }
  }

  /**
   * Compute the differences between our version and the canonical version of the project.
   */
  function compute_differences() {
    // Make sure we've hashed both remote and local files.
    $this->hash_remote_project();
    $this->hash_local_project();

    $results = array(
      'same' => array(),
      'different' => array(),
      'missing' => array(),
      'access_denied' => array(),
    );

    // Now compare the two file groups.
    foreach ($this->remote_files->files as $file) {
      if ($this->remote_files->files_hashes[$file] == $this->local_files->files_hashes[$file]) {
        $results['same'][] = $file;
      }
      elseif (!$this->local_files->file_exists($file)) {
        $results['missing'][] = $file;
      }
      elseif (!$this->local_files->is_readable($file)) {
        $results['access_denied'][] = $file;
      }
      else {
        $results['different'][] = $file;
      }
    }

    $this->result = $results;
  }

  /**
   * Return a nice report, a simple overview of the status of this project.
   */
  function compute_report() {
    // Ensure we know the differences.
    $this->compute_differences();

    // Do some counting

    $report = array(
      'project_name' => $this->name,
      'status' => HACKED_STATUS_UNCHECKED,
      'counts' => array(
        'same' => count($this->result['same']),
        'different' => count($this->result['different']),
        'missing' => count($this->result['missing']),
        'access_denied' => count($this->result['access_denied']),
      ),
      'title' => $this->title(),
    );

    // Add more details into the report result (if we can).
    $details = array(
      'link',
      'name',
      'existing_version',
      'install_type',
      'datestamp',
      'project_type',
      'includes',
    );
    foreach ($details as $item) {
      if (isset($this->project_info[$item])) {
        $report[$item] = $this->project_info[$item];
      }
    }


    if ($report['counts']['access_denied'] > 0) {
      $report['status'] = HACKED_STATUS_PERMISSION_DENIED;
    }
    elseif ($report['counts']['missing'] > 0) {
      $report['status'] = HACKED_STATUS_HACKED;
    }
    elseif ($report['counts']['different'] > 0) {
      $report['status'] = HACKED_STATUS_HACKED;
    }
    elseif ($report['counts']['same'] > 0) {
      $report['status'] = HACKED_STATUS_UNHACKED;
    }

    return $report;

  }

  /**
   * Return a nice detailed report.
   */
  function compute_details() {
    // Ensure we know the differences.
    $report = $this->compute_report();

    $report['files'] = array();

    // Add extra details about every file.
    $states = array(
      'access_denied' => HACKED_STATUS_PERMISSION_DENIED,
      'missing' => HACKED_STATUS_DELETED,
      'different' => HACKED_STATUS_HACKED,
      'same' => HACKED_STATUS_UNHACKED,
    );

    foreach ($states as $state => $status) {
      foreach ($this->result[$state] as $file) {
        $report['files'][$file] = $status;
        $report['diffable'][$file] = $this->file_is_diffable($file);
      }
    }

    return $report;

  }


  function file_is_diffable($file) {
    $this->hash_remote_project();
    $this->hash_local_project();
    return $this->remote_files->is_not_binary($file) && $this->local_files->is_not_binary($file);
  }

  function file_get_location($storage = 'local', $file) {
    switch ($storage) {
      case 'remote':
        $this->download_remote_project();
        return $this->remote_files->file_get_location($file);
      case 'local':
        $this->hash_local_project();
        return $this->local_files->file_get_location($file);
    }
    return FALSE;
  }



}

/**
 * Base class for downloading remote versions of projects.
 */
class hackedProjectWebDownloader {
  var $project;

  /**
   * Constructor, pass in the project this downloaded is expected to download.
   */
  function hackedProjectWebDownloader(&$project) {
    $this->project = $project;
  }

  /**
   * Returns a temp directory to work in.
   *
   * @param $namespace
   *   The optional namespace of the temp directory, defaults to the classname.
   */
  function get_temp_directory($namespace = NULL) {
    if (is_null($namespace)) {
      $namespace = get_class($this);
    }
    $segments = array(
      file_directory_temp(),
      'hacked-cache-' . get_current_user(),
      $namespace,
    );
    $dir = implode('/', array_filter($segments));
    if (!file_prepare_directory($dir, FILE_CREATE_DIRECTORY) && !mkdir($dir, 0775, TRUE)) {
      watchdog('hacked', 'Failed to create temp directory: %dir', array('%dir' => $dir), WATCHDOG_ERROR);
      return FALSE;
    }
    return $dir;
  }

  /**
   * Returns a directory to save the downloaded project into.
   */
  function get_destination() {
    $type = $this->project->project_type;
    $name = $this->project->name;
    $version = $this->project->existing_version;

    $dir = $this->get_temp_directory() .  "/$type/$name";
    // Build the destination folder tree if it doesn't already exists.
    if (!file_prepare_directory($dir, FILE_CREATE_DIRECTORY) && !mkdir($dir, 0775, TRUE)) {
      watchdog('hacked', 'Failed to create temp directory: %dir', array('%dir' => $dir), WATCHDOG_ERROR);
      return FALSE;
    }
    return "$dir/$version";
  }

  /**
   * Returns the final destination of the unpacked project.
   */
  function get_final_destination() {
    $dir = $this->get_destination();
    $name = $this->project->name;
    $version = $this->project->existing_version;
    $type = $this->project->project_type;
    // More special handling for core:
    if ($type != 'core') {
      $module_dir = $dir . "/$name";
    }
    else {
      $module_dir = $dir . '/' . $name . '-' . $version;
    }
    return $module_dir;
  }

  /**
   * Download the remote files to the local filesystem.
   */
  function download() {

  }

  /**
    * Recursively delete all files and folders in the specified filepath, then
    * delete the containing folder.
    *
    * Note that this only deletes visible files with write permission.
    *
    * @param string $path
    *   A filepath relative to file_directory_path.
    */
   function remove_dir($path) {
     if (is_file($path) || is_link($path)) {
       unlink($path);
     }
     elseif (is_dir($path)) {
       $d = dir($path);
       while (($entry = $d->read()) !== FALSE) {
         if ($entry == '.' || $entry == '..') continue;
         $entry_path = $path .'/'. $entry;
         $this->remove_dir($entry_path);
       }
       $d->close();
       rmdir($path);
     }
     else {
       watchdog('hacked', 'Unknown file type(%path) stat: %stat ',
                 array('%path' => $path,  '%stat' => print_r(stat($path),1)), WATCHDOG_ERROR);
     }
  }

}


/**
 * Downloads a project using a standard Drupal method.
 */
class hackedProjectWebFilesDownloader extends hackedProjectWebDownloader {

  function download_link() {
    if (!empty($this->project->project_info['releases'][$this->project->existing_version])) {
      $this_release = $this->project->project_info['releases'][$this->project->existing_version];
      return $this_release['download_link'];
    }
  }

  function download() {
    $dir = $this->get_destination();
    if (!($release_url = $this->download_link())) {
      return FALSE;
    }

    // If our directory already exists, we can just return the path to this cached version
    if (file_exists($dir) && count(hacked_file_scan_directory($dir, '/.*/', array('.', '..', 'CVS', '.svn', '.git')))) {
      return $dir;
    }

    // Build the destination folder tree if it doesn't already exists.
    if (!file_prepare_directory($dir, FILE_CREATE_DIRECTORY) && !mkdir($dir, 0775, TRUE)) {
      watchdog('hacked', 'Failed to create temp directory: %dir', array('%dir' => $dir), WATCHDOG_ERROR);
      return FALSE;
    }

    if (!($local_file = $this->file_get($release_url))) {
      watchdog('hacked', 'Could not download the project: @name from URL: @url', array('@name' => $this->project->title(), '@url' => $release_url), WATCHDOG_ERROR);
      return FALSE;
    }
    try {
      $this->archive_extract($local_file, $dir);
    }
    catch (Exception $e) {
      watchdog('hacked', 'Could not extract the project: @name. Error was: !error', array('@name' => $this->project->title(), '!error' => $e->getMessage()), WATCHDOG_ERROR);
      return FALSE;
    }

    return TRUE;
  }

  /**
   * Copies a file from $url to the temporary directory for updates.
   *
   * If the file has already been downloaded, returns the the local path.
   *
   * @param $url
   *   The URL of the file on the server.
   *
   * @return string
   *   Path to local file.
   */
  function file_get($url) {
    $parsed_url = parse_url($url);
    $remote_schemes = array('http', 'https', 'ftp', 'ftps', 'smb', 'nfs');
    if (!in_array($parsed_url['scheme'], $remote_schemes)) {
      // This is a local file, just return the path.
      return drupal_realpath($url);
    }

    // Check the cache and download the file if needed.
    $cache_directory = 'temporary://hacked-cache';
    $local = $cache_directory . '/' . basename($parsed_url['path']);

    if (!file_exists($cache_directory)) {
      mkdir($cache_directory);
    }

    return system_retrieve_file($url, $local, FALSE, FILE_EXISTS_REPLACE);
  }

  /**
   * Unpack a downloaded archive file.
   *
   * @param string $project
   *   The short name of the project to download.
   * @param string $file
   *   The filename of the archive you wish to extract.
   * @param string $directory
   *   The directory you wish to extract the archive into.
   * @return Archiver
   *   The Archiver object used to extract the archive.
   * @throws Exception on failure.
   */
  function archive_extract($file, $directory) {
    $archiver = archiver_get_archiver($file);
    if (!$archiver) {
      throw new Exception(t('Cannot extract %file, not a valid archive.', array ('%file' => $file)));
    }

    // Remove the directory if it exists, otherwise it might contain a mixture of
    // old files mixed with the new files (e.g. in cases where files were removed
    // from a later release).
    $files = $archiver->listContents();
    // Unfortunately, we can only use the directory name for this. :(
    $project = drupal_substr($files[0], 0, -1);
    $extract_location = $directory . '/' . $project;
    if (file_exists($extract_location)) {
      file_unmanaged_delete_recursive($extract_location);
    }

    $archiver->extract($directory);
    return $archiver;
  }


}

/**
 * Downloads a project using GIT.
 */
class hackedProjectWebGITDownloader extends hackedProjectWebDownloader {

  var $base_filename = '';

  function hackedProjectWebGITDownloader(&$project, $base_filename) {
    parent::hackedProjectWebDownloader($project);
    $this->base_filename = $base_filename;
  }

  /**
   * Get information about the GIT location of the project.
   */
  function download_link() {
    $info = array();
    $pinfo = &$this->project->project_info;
    // Special handling for core.
    if ($pinfo['project_type'] == 'core') {
      $info['giturl'] = 'http://git.drupal.org/project/' . $pinfo['name'] . '.git';
      $info['module'] = 'drupal';
      $info['checkout_folder'] = $pinfo['name'] . '-' . $pinfo['info']['version'];
      $info['tag'] = $this->git_deploy_get_tag($this->base_filename);
      $info['branch'] = $this->git_deploy_get_branch($this->base_filename);
    }
    else {
      $info['giturl'] = 'http://git.drupal.org/project/' . $pinfo['name'] . '.git';
      $info['module'] = $pinfo['name'];
      $info['checkout_folder'] = $pinfo['name'];
      $info['tag'] = $this->git_deploy_get_tag($this->base_filename);
      $info['branch'] = $this->git_deploy_get_branch($this->base_filename);
    }

    return $info;
  }
  
  /**
   * Get the Git branch the repo is currently on
   */
  function git_deploy_get_branch($file) {
    $proj_dir = dirname($file);
    if (is_dir($proj_dir)) {
      exec("cd $proj_dir; git symbolic-ref HEAD 2>/dev/null | cut -d\"/\" -f 3", $output_lines, $return_value);
      if ($return_value == 0) {
        return $output_lines[0];
      }
    }
  }
  
  /**
   * Get the GIT tag associated with the given file.
   */
  function git_deploy_get_tag($file) {
    $version = '';
    static $available = array();
    $match = array();
    if (empty($version)) {
      // The .info file contains no version data. Find the version based
      // on the sticky tag in the local workspace (the CVS/Tag file).
      $proj_dir = dirname($file);
      if (is_dir($proj_dir)) {
        exec("cd $proj_dir; git describe --tags", $output_lines, $return_value);
        if ($return_value == 0) {
          $version = $output_lines;
        }
      }
      /*
      $git_dir = dirname($file) .'/.git';
      if (is_dir($git_dir)) {
        $tag = '';  // If there's no Tag file, there's no tag, a.k.a. HEAD.
        if (file_exists($git_dir .'/Tag')) {
          $tag_file = trim(file_get_contents($git_dir .'/Tag'));
          if ($tag_file) {
            // Get the sticky tag for this workspace: strip off the leading 'T'.
            $tag = preg_replace('@^(T|N)@', '', $tag_file);
          }
        }
        $version = $tag;
      }
      */
    }
    
    if (empty($version)) {
      $version = 'HEAD';
    }

    return $version;
  }

  function download() {
    $dir = $this->get_destination();
    $release_info = $this->download_link();

    // TODO: Only delete if not a TAG.
    if (file_exists($dir)) {
      if ($this->is_git_tag($release_info['tag'])) {
        return $dir;
      }
      else {
        // This is not a GIT tag, so we need to re-download.
        $this->remove_dir($dir);
      }
    }

    if (hacked_git_checkout($release_info['giturl'], $release_info['module'], $dir, $release_info['checkout_folder'], $release_info['branch'], $release_info['tag'])) {
      return $dir;
    }

    // Something went wrong:
    return FALSE;
  }

  function get_destination() {
    $type = $this->project->project_type;
    $name = $this->project->name;
    $info = $this->download_link();
    // Add in the GIT tag here.
    $version = $this->project->existing_version . '-' . $info['tag'];


    $dir = $this->get_temp_directory() . "/$type/$name";
    // Build the destination folder tree if it doesn't already exists.
    if (!file_prepare_directory($dir, FILE_CREATE_DIRECTORY) && !mkdir($dir, 0775, TRUE)) {
      watchdog('hacked', 'Failed to create temp directory: %dir', array('%dir' => $dir), WATCHDOG_ERROR);
      return FALSE;
    }
    return "$dir/$version";
  }
  
  function is_git_tag($tag) {
    // TODO: Add tag validation
    return TRUE;
  }
}

/**
 * Downloads a project using CVS.
 *
 * @deprecated
 */
class hackedProjectWebCVSDownloader extends hackedProjectWebDownloader {

  var $base_filename = '';

  function hackedProjectWebCVSDownloader(&$project, $base_filename) {
    parent::hackedProjectWebDownloader($project);
    $this->base_filename = $base_filename;
  }

  /**
   * Get information about the CVS location of the project.
   */
  function download_link() {
    $info = array();
    $pinfo = &$this->project->project_info;
    // Special handling for core.
    if ($pinfo['project_type'] == 'core') {
      $info['cvsroot'] = ':pserver:anonymous:anonymous@cvs.drupal.org:/cvs/drupal';
      $info['module'] = 'drupal';
      $info['checkout_folder'] = $pinfo['name'] . '-' . $pinfo['info']['version'];
      $info['tag'] = $this->cvs_deploy_get_tag($this->base_filename);
    }
    else {
      $info['cvsroot'] = ':pserver:anonymous:anonymous@cvs.drupal.org:/cvs/drupal-contrib';
      $info['module'] = 'contributions/' . $pinfo['project_type'] . 's/' . $pinfo['name'];
      $info['checkout_folder'] = $pinfo['name'];
      $info['tag'] = $this->cvs_deploy_get_tag($this->base_filename);
    }

    return $info;
  }

  /**
   * Get the CVS tag associated with the given file.
   */
  function cvs_deploy_get_tag($file) {
    $version = '';
    static $available = array();
    $match = array();
    if (empty($version)) {
      // The .info file contains no version data. Find the version based
      // on the sticky tag in the local workspace (the CVS/Tag file).
      $cvs_dir = dirname($file) .'/CVS';
      if (is_dir($cvs_dir)) {
        $tag = '';  // If there's no Tag file, there's no tag, a.k.a. HEAD.
        if (file_exists($cvs_dir .'/Tag')) {
          $tag_file = trim(file_get_contents($cvs_dir .'/Tag'));
          if ($tag_file) {
            // Get the sticky tag for this workspace: strip off the leading 'T'.
            $tag = preg_replace('@^(T|N)@', '', $tag_file);
          }
        }
        $version = $tag;
      }
    }
    // The weird concatenation prevents CVS from 'expanding' this $Name.
    elseif (preg_match('/\$'.'Name: (.*?)\$/', $version, $match)) {
      $version = trim($match[1]);
    }
    if (empty($version)) {
      $version = 'HEAD';
    }

    return $version;
  }

  function download() {
    $dir = $this->get_destination();
    $release_info = $this->download_link();

    // TODO: Only delete if not a TAG.
    if (file_exists($dir)) {
      if ($this->is_cvs_tag($release_info['tag'])) {
        return $dir;
      }
      else {
        // This is not a CVS tag, so we need to re-download.
        $this->remove_dir($dir);
      }
    }

    if (hacked_cvs_checkout($release_info['cvsroot'], $release_info['module'], $dir, $release_info['checkout_folder'], $release_info['tag'])) {
      return $dir;
    }

    // Something went wrong:
    return FALSE;
  }

  function get_destination() {
    $type = $this->project->project_type;
    $name = $this->project->name;
    $info = $this->download_link();
    // Add in the CVS tag here.
    $version = $this->project->existing_version . '-' . $info['tag'];


    $dir = $this->get_temp_directory() . "/$type/$name";
    // Build the destination folder tree if it doesn't already exists.
    if (!file_prepare_directory($dir, FILE_CREATE_DIRECTORY) && !mkdir($dir, 0775, TRUE)) {
      watchdog('hacked', 'Failed to create temp directory: %dir', array('%dir' => $dir), WATCHDOG_ERROR);
      return FALSE;
    }
    return "$dir/$version";
  }

  function is_cvs_tag($tag) {
    // CVS tags in Drupal are of the form:
    $valid_tags = '@^DRUPAL-[567]--(\d+)-(\d+)(-[A-Z0-9]+)?@';
    return (bool)preg_match($valid_tags, $tag);
  }
}

/**
 * Represents a group of files on the local filesystem.
 */
class hackedFileGroup {

  var $base_path = '';
  var $files = array();
  var $files_hashes = array();
  var $file_mtimes = array();

  var $hasher;

  /**
   * Constructor.
   */
  function hackedFileGroup($base_path) {
    $this->base_path = $base_path;
    $this->hasher = hacked_get_file_hasher();
  }

  /**
   * Return a new hackedFileGroup listing all files inside the given $path.
   */
  static function fromDirectory($path) {
    $filegroup = new hackedFileGroup($path);
    // Find all the files in the path, and add them to the file group.
    $filegroup->scan_base_path();
    return $filegroup;
  }

  /**
   * Return a new hackedFileGroup listing all files specified.
   */
  static function fromList($path, $files) {
    $filegroup = new hackedFileGroup($path);
    // Find all the files in the path, and add them to the file group.
    $filegroup->files = $files;
    return $filegroup;
  }

  /**
   * Locate all sensible files at the base path of the file group.
   */
  function scan_base_path() {
    $files = hacked_file_scan_directory($this->base_path, '/.*/', array('.', '..', 'CVS', '.svn', '.git'));
    foreach ($files as $file) {
      $filename = str_replace($this->base_path . '/', '', $file->filename);
      $this->files[] = $filename;
    }
  }

  /**
   * Hash all files listed in the file group.
   */
  function compute_hashes() {
    foreach ($this->files as $filename) {
      $this->files_hashes[$filename] = $this->hasher->hash($this->base_path . '/' .$filename);
    }
  }

  /**
   * Determine if the given file is readable.
   */
  function is_readable($file) {
    return is_readable($this->base_path . '/' . $file);
  }

  /**
   * Determine if a file exists.
   */
  function file_exists($file) {
    return file_exists($this->base_path . '/' . $file);
  }

  /**
   * Determine if the given file is binary.
   */
  function is_not_binary($file) {
    return is_readable($this->base_path . '/' . $file) && !hacked_file_is_binary($this->base_path . '/' . $file);
  }

  function file_get_location($file) {
    return $this->base_path . '/' . $file;
  }

}

/**
 * Base class for the different ways that files can be hashed.
 */
class hackedFileHasher {
  /**
   * Returns a hash of the given filename.
   *
   * Ignores file line endings
   */
  function hash($filename) {
    if (file_exists($filename)) {
      if ($hash = $this->cache_get($filename)) {
        return $hash;
      }
      else {
        $hash = $this->perform_hash($filename);
        $this->cache_set($filename, $hash);
        return $hash;
      }
    }
  }

  function cache_set($filename, $hash) {
    cache_set($this->cache_key($filename), $hash, HACKED_CACHE_TABLE, strtotime('+7 days'));
  }

  function cache_get($filename) {
    $cache = cache_get($this->cache_key($filename), HACKED_CACHE_TABLE);
    if (!empty($cache->data)) {
      return $cache->data;
    }
  }

  function perform_hash($filename) {
    return '';
  }



  function cache_key($filename) {
    $key = array(
      'filename' => $filename,
      'mtime' => filemtime($filename),
      'class_name' => get_class($this),
    );
    return sha1(serialize($key));
  }
}

class hackedFileIgnoreEndingsHasher extends hackedFileHasher {
  /**
   * Returns a hash of the given filename.
   *
   * Ignores file line endings.
   */
  function perform_hash($filename) {
    if (!hacked_file_is_binary($filename)) {
      $file = file($filename, FILE_IGNORE_NEW_LINES);
      return sha1(serialize($file));
    }
    else {
      return sha1_file($filename);
    }
  }
}

/**
 * This is a much faster, but potentially less useful file hasher.
 */
class hackedFileIncludeEndingsHasher extends hackedFileHasher {
  function perform_hash($filename) {
    return sha1_file($filename);
  }
}
